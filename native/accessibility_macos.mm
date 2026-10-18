#include "accessibility.hpp"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <QApplication>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <utility>

namespace {
constexpr qsizetype maximumText = 64 * 1024;

class Attribute {
    CFTypeRef value = nullptr;

  public:
    Attribute(AXUIElementRef element, CFStringRef name) {
        AXUIElementCopyAttributeValue(element, name, &value);
    }
    ~Attribute() {
        if (value)
            CFRelease(value);
    }
    Attribute(const Attribute &) = delete;
    Attribute &operator=(const Attribute &) = delete;
    CFTypeRef get() const { return value; }
    bool is(CFTypeID type) const { return value && CFGetTypeID(value) == type; }
};

QString stringValue(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID())
        return {};
    auto string = static_cast<CFStringRef>(value);
    const auto size = CFStringGetLength(string);
    if (size < 0 || size > maximumText)
        return {};
    QString result(size, Qt::Uninitialized);
    CFStringGetCharacters(string, CFRangeMake(0, size), reinterpret_cast<UniChar *>(result.data()));
    return result.toUtf8().size() <= maximumText ? result : QString();
}

bool secureElement(AXUIElementRef element) {
    if (IsSecureEventInputEnabled())
        return true;
    Attribute subrole(element, kAXSubroleAttribute);
    if (subrole.get() && CFEqual(subrole.get(), kAXSecureTextFieldSubrole))
        return true;
    // Optional on web accessibility implementations; unknown attributes are
    // harmless, and native secure fields are covered by the standard subrole.
    Attribute protectedContent(element, CFSTR("AXProtectedContent"));
    return protectedContent.is(CFBooleanGetTypeID()) &&
           CFBooleanGetValue(static_cast<CFBooleanRef>(protectedContent.get()));
}
} // namespace

// AX attribute queries can block when another app is unresponsive. Keep them off
// the UI thread, bound their messaging timeout, and allow only one read at a time.
class MacSelectionReader : public QObject {
    Q_OBJECT
  public:
    void read(pid_t pid, quint64 epoch) {
        if (!AXIsProcessTrusted()) {
            emit ready(epoch, {}, 4);
            return;
        }
        if (IsSecureEventInputEnabled()) {
            emit ready(epoch, {}, 2);
            return;
        }
        auto app = AXUIElementCreateApplication(pid);
        if (!app) {
            emit ready(epoch, {}, 3);
            return;
        }
        AXUIElementSetMessagingTimeout(app, 0.15f);
        QString text;
        int status = 1;
        {
            Attribute focus(app, kAXFocusedUIElementAttribute);
            if (focus.is(AXUIElementGetTypeID())) {
                auto element = static_cast<AXUIElementRef>(const_cast<void *>(focus.get()));
                AXUIElementSetMessagingTimeout(element, 0.15f);
                if (secureElement(element)) {
                    status = 2;
                } else {
                    Attribute selectedRange(element, kAXSelectedTextRangeAttribute);
                    CFRange range{};
                    const bool hasRange =
                        selectedRange.is(AXValueGetTypeID()) &&
                        AXValueGetType(static_cast<AXValueRef>(
                            const_cast<void *>(selectedRange.get()))) == kAXValueCFRangeType &&
                        AXValueGetValue(
                            static_cast<AXValueRef>(const_cast<void *>(selectedRange.get())),
                            kAXValueCFRangeType, &range);
                    if (hasRange &&
                        (range.location < 0 || range.length < 0 || range.length > maximumText)) {
                        status = 0;
                    } else {
                        Attribute selection(element, kAXSelectedTextAttribute);
                        if (selection.is(CFStringGetTypeID())) {
                            text = stringValue(selection.get());
                            status = 0;
                        }
                    }
                }
            }
        }
        CFRelease(app);
        emit ready(epoch, text, status);
    }
  signals:
    void ready(quint64 epoch, const QString &text, int status);
};

class AccessibilitySelection::Impl {
  public:
    AccessibilitySelection *owner;
    bool armed = false, reading = false, readAgain = false, baselinePending = true;
    quint64 generation = 0, epoch = 0;
    pid_t applicationPid = 0;
    AXUIElementRef application = nullptr;
    AXObserverRef observer = nullptr;
    id activationObserver = nil;
    QThread workerThread;
    MacSelectionReader *reader = new MacSelectionReader;
    QTimer poll, settle;
    QString lastText, candidate;

    explicit Impl(AccessibilitySelection *owner) : owner(owner) {
        reader->moveToThread(&workerThread);
        QObject::connect(&workerThread, &QThread::finished, reader, &QObject::deleteLater);
        QObject::connect(
            reader, &MacSelectionReader::ready, owner,
            [this](quint64 token, const QString &text, int status) {
                reading = false;
                const bool repeat = std::exchange(readAgain, false);
                if (armed && (repeat || token != epoch))
                    QTimer::singleShot(0, owner, [this] { requestRead(); });
                if (!armed || token != epoch)
                    return;
                if (status == 4) {
                    stop();
                    emit owner->availability(
                        false,
                        "Accessibility permission was revoked. Allow Ranslator in System Settings "
                        "→ Privacy & Security → Accessibility, then invoke selection again.",
                        generation);
                    return;
                }
                if (status != 0) {
                    candidate.clear();
                    lastText.clear();
                    settle.stop();
                    return;
                }
                if (baselinePending) {
                    baselinePending = false;
                    lastText = text;
                    return;
                }
                if (text == lastText)
                    return;
                lastText = text;
                candidate = text;
                if (candidate.trimmed().isEmpty())
                    settle.stop();
                else
                    settle.start();
            });
        workerThread.start();
        poll.setInterval(240);
        QObject::connect(&poll, &QTimer::timeout, owner, [this] {
            followFrontmost();
            requestRead();
        });
        settle.setSingleShot(true);
        settle.setInterval(140);
        QObject::connect(&settle, &QTimer::timeout, owner, [this] {
            if (!armed || candidate.isEmpty() || QApplication::activeWindow())
                return;
            if (reading || readAgain || [NSEvent pressedMouseButtons] != 0) {
                settle.start(60);
                return;
            }
            const auto text = candidate;
            candidate.clear();
            emit owner->selected(text, generation);
        });
    }
    ~Impl() {
        stop();
        workerThread.quit();
        workerThread.wait(); // One bounded AX query; no detached work outlives the adapter.
    }
    static void changed(AXObserverRef, AXUIElementRef, CFStringRef notification, void *context) {
        auto self = static_cast<Impl *>(context);
        if (self->armed) {
            if (CFEqual(notification, kAXSelectedTextChangedNotification))
                self->baselinePending = false;
            self->requestRead();
        }
    }
    void clearApplication() {
        ++epoch;
        settle.stop();
        lastText.clear();
        candidate.clear();
        baselinePending = true;
        if (observer) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(observer),
                                  kCFRunLoopCommonModes);
            if (application) {
                AXObserverRemoveNotification(observer, application,
                                             kAXFocusedUIElementChangedNotification);
                AXObserverRemoveNotification(observer, application,
                                             kAXSelectedTextChangedNotification);
            }
            CFRelease(observer);
            observer = nullptr;
        }
        if (application) {
            CFRelease(application);
            application = nullptr;
        }
        applicationPid = 0;
    }
    void stop() {
        armed = false;
        poll.stop();
        clearApplication();
        if (activationObserver) {
            [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:activationObserver];
            activationObserver = nil;
        }
    }
    void followFrontmost() {
        if (!armed)
            return;
        const auto app = [[NSWorkspace sharedWorkspace] frontmostApplication];
        const pid_t pid = app ? [app processIdentifier] : 0;
        if (pid == applicationPid)
            return;
        clearApplication();
        if (pid <= 0 || pid == QCoreApplication::applicationPid())
            return;
        applicationPid = pid;
        application = AXUIElementCreateApplication(pid);
        if (!application)
            return;
        AXUIElementSetMessagingTimeout(application, 0.15f);
        if (AXObserverCreate(pid, changed, &observer) == kAXErrorSuccess) {
            AXObserverAddNotification(observer, application, kAXFocusedUIElementChangedNotification,
                                      this);
            AXObserverAddNotification(observer, application, kAXSelectedTextChangedNotification,
                                      this);
            CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(observer),
                               kCFRunLoopCommonModes);
        }
        // Some apps do not emit AXSelectedTextChanged. The scoped poll provides
        // the same selection path while the user has armed translation.
        requestRead();
    }
    void requestRead() {
        if (!armed || applicationPid <= 0 || QApplication::activeWindow())
            return;
        if (reading) {
            readAgain = true;
            return;
        }
        reading = true;
        const auto pid = applicationPid;
        const auto token = epoch;
        QMetaObject::invokeMethod(
            reader, [worker = reader, pid, token] { worker->read(pid, token); },
            Qt::QueuedConnection);
    }
    void arm(bool value, quint64 nextGeneration) {
        if (armed == value && generation == nextGeneration)
            return;
        stop();
        generation = nextGeneration;
        if (!value)
            return;
        const void *keys[] = {kAXTrustedCheckOptionPrompt};
        const void *values[] = {kCFBooleanTrue};
        auto options =
            CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        const bool trusted = AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
        if (!trusted) {
            emit owner->availability(
                false,
                "Allow Ranslator in System Settings → Privacy & Security → Accessibility, then "
                "invoke selection again. Snapshot uses Screen Recording permission separately.",
                generation);
            return;
        }
        armed = true;
        const QPointer<AccessibilitySelection> guard(owner);
        activationObserver = [[[NSWorkspace sharedWorkspace] notificationCenter]
            addObserverForName:NSWorkspaceDidActivateApplicationNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *notification) {
                      (void)notification;
                      if (guard && guard->impl->armed)
                          guard->impl->followFrontmost();
                    }];
        followFrontmost();
        poll.start();
        emit owner->availability(true,
                                 "Listening for selected text in accessible applications. Use "
                                 "Snapshot for images or unsupported applications.",
                                 generation);
    }
};

AccessibilitySelection::AccessibilitySelection(QObject *parent, QString)
    : QObject(parent), impl(std::make_unique<Impl>(this)) {}
AccessibilitySelection::~AccessibilitySelection() = default;
void AccessibilitySelection::setArmed(bool value, quint64 generation) {
    impl->arm(value, generation);
}
bool AccessibilitySelection::isArmed() const { return impl->armed; }

#include "accessibility_macos.moc"
