#include "macos.hpp"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/QuartzCore.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <ServiceManagement/ServiceManagement.h>
#include <algorithm>
#import <objc/runtime.h>
#include <optional>

namespace {
constexpr OSType signature = 0x52616e73; // Rans
constexpr CGEventFlags modifierMask = kCGEventFlagMaskShift | kCGEventFlagMaskControl |
                                      kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand;

std::optional<CGEventFlags> modifierChord(const QString &sequence) {
    CGEventFlags flags = 0;
    for (const auto &part : sequence.split('+')) {
        const auto token = part.trimmed().toLower();
        if (token == "shift")
            flags |= kCGEventFlagMaskShift;
        else if (token == "ctrl" || token == "control")
            flags |= kCGEventFlagMaskControl;
        else if (token == "alt")
            flags |= kCGEventFlagMaskAlternate;
        else if (token == "meta" || token == "super")
            flags |= kCGEventFlagMaskCommand;
        else
            return {};
    }
    return flags ? std::optional<CGEventFlags>(flags) : std::nullopt;
}

std::optional<UInt32> keyCode(Qt::Key key) {
    const QHash<int, UInt32> special = {{Qt::Key_Return, kVK_Return},
                                        {Qt::Key_Enter, kVK_ANSI_KeypadEnter},
                                        {Qt::Key_Space, kVK_Space},
                                        {Qt::Key_Tab, kVK_Tab},
                                        {Qt::Key_Escape, kVK_Escape},
                                        {Qt::Key_Backspace, kVK_Delete},
                                        {Qt::Key_Delete, kVK_ForwardDelete},
                                        {Qt::Key_Left, kVK_LeftArrow},
                                        {Qt::Key_Right, kVK_RightArrow},
                                        {Qt::Key_Up, kVK_UpArrow},
                                        {Qt::Key_Down, kVK_DownArrow},
                                        {Qt::Key_Home, kVK_Home},
                                        {Qt::Key_End, kVK_End},
                                        {Qt::Key_PageUp, kVK_PageUp},
                                        {Qt::Key_PageDown, kVK_PageDown}};
    if (special.contains(key))
        return special[key];
    const UInt32 functions[] = {kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
                                kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
                                kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20)
        return functions[key - Qt::Key_F1];
    // Resolve characters through the user's layout, including non-US keyboards. The
    // ASCII-capable layout keeps saved Latin chords usable while an IME is active.
    TISInputSourceRef layouts[] = {TISCopyCurrentKeyboardLayoutInputSource(),
                                   TISCopyCurrentASCIICapableKeyboardLayoutInputSource()};
    std::optional<UInt32> result;
    for (auto source : layouts) {
        if (!source)
            continue;
        auto data = static_cast<CFDataRef>(
            TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
        if (data && !result) {
            auto layout = reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(data));
            for (UInt32 code = 0; code < 128; ++code) {
                UInt32 dead = 0;
                UniChar chars[4];
                UniCharCount length = 0;
                if (UCKeyTranslate(layout, code, kUCKeyActionDown, 0, LMGetKbdType(),
                                   kUCKeyTranslateNoDeadKeysBit, &dead, 4, &length,
                                   chars) == noErr &&
                    length == 1 && QChar(chars[0]).toUpper().unicode() == static_cast<uint>(key)) {
                    result = code;
                    break;
                }
            }
        }
        CFRelease(source);
    }
    return result;
}

struct Display {
    CGDirectDisplayID id;
    QRect geometry;
    qreal scale;
};
std::optional<Display> displayFor(QScreen *screen) {
    if (!screen)
        return {};
    const auto originHeight = NSScreen.screens.firstObject.frame.size.height;
    for (NSScreen *native in NSScreen.screens) {
        const auto frame = native.frame;
        QRect rect(qRound(frame.origin.x), qRound(originHeight - NSMaxY(frame)),
                   qRound(frame.size.width), qRound(frame.size.height));
        if (rect == screen->geometry())
            return Display{[native.deviceDescription[@"NSScreenNumber"] unsignedIntValue],
                           rect,
                           native.backingScaleFactor};
    }
    return {};
}

QImage imageFrom(CGImageRef image) {
    const int width = static_cast<int>(CGImageGetWidth(image));
    const int height = static_cast<int>(CGImageGetHeight(image));
    if (width <= 0 || height <= 0 || qint64(width) * height > 100000000)
        return {};
    QImage copy(width, height, QImage::Format_ARGB32_Premultiplied);
    if (copy.isNull())
        return {};
    copy.fill(Qt::transparent);
    auto color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    auto context =
        CGBitmapContextCreate(copy.bits(), width, height, 8, copy.bytesPerLine(), color,
                              kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(color);
    if (!context)
        return {};
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    return copy;
}
} // namespace

struct MacShortcuts::Impl {
    MacShortcuts *owner;
    EventHandlerRef handler = nullptr;
    QList<EventHotKeyRef> keys;
    QHash<UInt32, QString> actions;
    QSet<UInt32> held;
    QHash<quint64, QString> modifiers;
    std::function<void(QString)> triggered;
    CFMachPortRef tap = nullptr;
    CFRunLoopSourceRef source = nullptr;
    CGEventFlags chord = 0;
    bool contaminated = false;
    CFAbsoluteTime started = 0;
    quint64 generation = 0;

    explicit Impl(MacShortcuts *object) : owner(object) {}
    ~Impl() { clear(); }
    void clear() {
        ++generation;
        for (auto key : keys)
            UnregisterEventHotKey(key);
        keys.clear();
        actions.clear();
        held.clear();
        modifiers.clear();
        if (handler) {
            RemoveEventHandler(handler);
            handler = nullptr;
        }
        if (source) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
            CFRelease(source);
            source = nullptr;
        }
        if (tap) {
            CFMachPortInvalidate(tap);
            CFRelease(tap);
            tap = nullptr;
        }
        chord = 0;
        contaminated = false;
    }
    void fire(const QString &id) {
        if (id.isEmpty())
            return;
        // Deliver outside the native callback: binding changes may remove the event tap.
        const auto token = generation;
        QTimer::singleShot(0, owner, [this, id, token] {
            if (triggered && token == generation)
                triggered(id);
        });
    }
    static OSStatus hotKey(EventHandlerCallRef, EventRef event, void *data) {
        EventHotKeyID id{};
        if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                              sizeof(id), nullptr, &id) != noErr ||
            id.signature != signature)
            return eventNotHandledErr;
        auto *self = static_cast<Impl *>(data);
        if (GetEventKind(event) == kEventHotKeyReleased) {
            self->held.remove(id.id);
            return noErr;
        }
        if (self->held.contains(id.id))
            return noErr;
        self->held.insert(id.id);
        self->fire(self->actions.value(id.id));
        return noErr;
    }
    static CGEventRef event(CGEventTapProxy, CGEventType type, CGEventRef value, void *data) {
        auto *self = static_cast<Impl *>(data);
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            self->chord = 0;
            self->contaminated = true;
            if (self->tap)
                CGEventTapEnable(self->tap, true);
            return value;
        }
        const auto flags = CGEventGetFlags(value) & modifierMask;
        if (type != kCGEventFlagsChanged) {
            self->contaminated = true;
            return value; // Never read, retain, or suppress typed characters.
        }
        if (flags) {
            if (!self->chord) {
                self->started = CFAbsoluteTimeGetCurrent();
                self->contaminated = false;
            }
            self->chord |= flags;
        } else {
            if (!self->contaminated && self->chord &&
                CFAbsoluteTimeGetCurrent() - self->started < .4)
                self->fire(self->modifiers.value(self->chord));
            self->chord = 0;
            self->contaminated = false;
        }
        return value;
    }
};

MacShortcuts::MacShortcuts(QObject *parent) : QObject(parent), impl(std::make_unique<Impl>(this)) {}
MacShortcuts::~MacShortcuts() = default;
void MacShortcuts::bind(const QJsonObject &config, std::function<void(QString)> triggered,
                        std::function<void(QString)> error) {
    impl->clear();
    impl->triggered = std::move(triggered);
    const EventTypeSpec types[] = {{kEventClassKeyboard, kEventHotKeyPressed},
                                   {kEventClassKeyboard, kEventHotKeyReleased}};
    if (InstallApplicationEventHandler(Impl::hotKey, 2, types, impl.get(), &impl->handler) !=
        noErr) {
        error("macOS could not register the global shortcut handler.");
        return;
    }
    UInt32 identifier = 0;
    for (const auto &id : {"once", "continuous", "snapshot"}) {
        const auto sequence = config[id].toObject()["sequence"].toString();
        if (const auto flags = modifierChord(sequence)) {
            impl->modifiers.insert(*flags, id);
            continue;
        }
        const auto key = QKeySequence::fromString(sequence, QKeySequence::PortableText);
        const auto code = key.count() == 1 ? keyCode(key[0].key()) : std::nullopt;
        if (!code) {
            error("Unsupported macOS shortcut: " + sequence);
            continue;
        }
        const auto flags = key[0].keyboardModifiers();
        UInt32 modifiers = 0;
        if (flags & Qt::ControlModifier)
            modifiers |= controlKey;
        if (flags & Qt::AltModifier)
            modifiers |= optionKey;
        if (flags & Qt::ShiftModifier)
            modifiers |= shiftKey;
        if (flags & Qt::MetaModifier)
            modifiers |= cmdKey;
        EventHotKeyRef reference = nullptr;
        EventHotKeyID hotkey = {signature, ++identifier};
        if (RegisterEventHotKey(*code, modifiers, hotkey, GetApplicationEventTarget(), 0,
                                &reference) != noErr) {
            error("Shortcut " + sequence +
                  " is unavailable or already in use. Choose another chord.");
            continue;
        }
        impl->keys.append(reference);
        impl->actions.insert(identifier, id);
    }
    if (impl->modifiers.isEmpty())
        return;
    if (!CGPreflightListenEventAccess() && !CGRequestListenEventAccess()) {
        error("Modifier-only shortcuts need Input Monitoring permission in System Settings → "
              "Privacy & Security. Ordinary key chords remain available. After granting access, "
              "reopen Ranslator.");
        return;
    }
    const auto events = CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(kCGEventKeyDown) |
                        CGEventMaskBit(kCGEventLeftMouseDown) |
                        CGEventMaskBit(kCGEventRightMouseDown) |
                        CGEventMaskBit(kCGEventOtherMouseDown);
    impl->tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionListenOnly, events, Impl::event, impl.get());
    if (!impl->tap) {
        error("macOS did not allow modifier listening. Check Input Monitoring permission, then "
              "reopen Ranslator.");
        return;
    }
    impl->source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, impl->tap, 0);
    if (impl->source)
        CFRunLoopAddSource(CFRunLoopGetMain(), impl->source, kCFRunLoopCommonModes);
    CGEventTapEnable(impl->tap, true);
}

namespace NativeMac {
void capture(QObject *owner, const QString &mode, QScreen *screen,
             std::function<void(QImage, QString)> done) {
    if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
        done({}, "Enable Screen Recording for Ranslator in System Settings → Privacy & Security, "
                 "then reopen the app and try Snapshot again.");
        return;
    }
    struct Job {
        QPointer<QObject> owner;
        QList<Display> displays;
        QRect bounds;
        qreal scale = 1;
        QImage canvas;
        int pending = 0;
        bool finished = false;
        std::function<void(QImage, QString)> done;
        void finish(QImage image, const QString &error) {
            if (finished)
                return;
            finished = true;
            canvas = {};
            if (owner)
                done(std::move(image), error);
        }
    };
    auto job = std::make_shared<Job>();
    job->owner = owner;
    job->done = std::move(done);
    const auto screens = mode == "desktop" ? QGuiApplication::screens() : QList<QScreen *>{screen};
    for (auto *item : screens) {
        auto display = displayFor(item);
        if (!display) {
            job->finish({}, "The selected display disconnected. Try Snapshot again.");
            return;
        }
        job->displays.append(*display);
        job->bounds = job->bounds.united(display->geometry);
        job->scale = std::max(job->scale, display->scale);
    }
    const QSize pixels(qRound(job->bounds.width() * job->scale),
                       qRound(job->bounds.height() * job->scale));
    if (pixels.isEmpty() || qint64(pixels.width()) * pixels.height() > 100000000) {
        job->finish(
            {},
            "The combined desktop is too large. Choose Current monitor in Screen text settings.");
        return;
    }
    job->canvas = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
    if (job->canvas.isNull()) {
        job->finish({}, "Not enough memory to capture this desktop.");
        return;
    }
    job->canvas.fill(Qt::black);
    job->pending = job->displays.size();
    QTimer::singleShot(20000, owner, [job] {
        job->finish({},
                    "Screen capture timed out. Check Screen Recording permission and try again.");
    });
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                   QMetaObject::invokeMethod(
                                       qApp,
                                       [job, content, error] {
                                           if (!job->owner || job->finished)
                                               return;
                                           if (error || !content) {
                                               job->finish(
                                                   {},
                                                   "Screen capture failed. Check Screen Recording "
                                                   "permission, then reopen Ranslator. " +
                                                       QString::fromNSString(
                                                           error.localizedDescription));
                                               return;
                                           }
                                           for (const auto &display : job->displays) {
                                               SCDisplay *match = nil;
                                               for (SCDisplay *candidate in content.displays)
                                                   if (candidate.displayID == display.id) {
                                                       match = candidate;
                                                       break;
                                                   }
                                               if (!match) {
                                                   job->finish({}, "The selected display is no "
                                                                   "longer available.");
                                                   return;
                                               }
                                               SCContentFilter *filter =
                                                   [[SCContentFilter alloc] initWithDisplay:match
                                                                           excludingWindows:@[]];
                                               SCStreamConfiguration *configuration =
                                                   [SCStreamConfiguration new];
                                               // CGDisplayPixelsWide counts points; on
                                               // Retina that halves the frame and blurs text.
                                               configuration.width = static_cast<size_t>(
                                                   qRound(display.geometry.width() *
                                                          display.scale));
                                               configuration.height = static_cast<size_t>(
                                                   qRound(display.geometry.height() *
                                                          display.scale));
                                               configuration.showsCursor = NO;
                                               [SCScreenshotManager
                                                   captureImageWithFilter:filter
                                                            configuration:configuration
                                                        completionHandler:^(CGImageRef result,
                                                                            NSError *captureError) {
                                                          QImage image =
                                                              result ? imageFrom(result) : QImage();
                                                          const auto message =
                                                              QString::fromNSString(
                                                                  captureError
                                                                      .localizedDescription);
                                                          QMetaObject::invokeMethod(
                                                              qApp,
                                                              [job, display, image, message] {
                                                                  if (!job->owner || job->finished)
                                                                      return;
                                                                  if (image.isNull()) {
                                                                      job->finish(
                                                                          {}, "Could not capture "
                                                                              "the display. " +
                                                                                  message);
                                                                      return;
                                                                  }
                                                                  const auto relative =
                                                                      display.geometry.translated(
                                                                          -job->bounds.topLeft());
                                                                  {
                                                                      QPainter painter(
                                                                          &job->canvas);
                                                                      painter.drawImage(
                                                                          QRectF(relative.x() *
                                                                                     job->scale,
                                                                                 relative.y() *
                                                                                     job->scale,
                                                                                 relative.width() *
                                                                                     job->scale,
                                                                                 relative.height() *
                                                                                     job->scale),
                                                                          image);
                                                                  }
                                                                  if (--job->pending == 0)
                                                                      job->finish(
                                                                          std::move(job->canvas),
                                                                          {});
                                                              },
                                                              Qt::QueuedConnection);
                                                        }];
                                           }
                                       },
                                       Qt::QueuedConnection);
                                 }];
}

bool applyBackdrop(QWidget *widget, bool enabled, bool dark) {
    if (QGuiApplication::platformName() != "cocoa")
        return false;
    NSView *view = (__bridge NSView *)(reinterpret_cast<void *>(widget->winId()));
    NSWindow *window = view.window;
    if (!window || !view.superview)
        return false;
    static char backdropKey;
    NSVisualEffectView *effect = objc_getAssociatedObject(window, &backdropKey);
    if (!effect && enabled) {
        // Qt owns its contentView and responder chain. Insert a sibling behind it;
        // replacing contentView would break Cocoa focus, resizing, and destruction.
        effect = [[NSVisualEffectView alloc] initWithFrame:view.frame];
        effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        effect.material = NSVisualEffectMaterialHUDWindow;
        effect.state = NSVisualEffectStateActive;
        [view.superview addSubview:effect positioned:NSWindowBelow relativeTo:view];
        objc_setAssociatedObject(window, &backdropKey, effect, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    if (effect) {
        effect.hidden = !enabled;
        effect.appearance =
            [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        effect.wantsLayer = YES;
        effect.layer.cornerRadius = 30;
        effect.layer.masksToBounds = YES;
        if (effect.superview != view.superview) {
            [effect removeFromSuperview];
            [view.superview addSubview:effect positioned:NSWindowBelow relativeTo:view];
        }
        effect.frame = [view convertRect:NSInsetRect(view.bounds, 20, 20) toView:view.superview];
    }
    window.opaque = NO;
    window.backgroundColor = NSColor.clearColor;
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    return enabled && effect != nil;
}

void prepareOverlay(QWidget *widget) {
    if (QGuiApplication::platformName() != "cocoa")
        return;
    NSView *view = (__bridge NSView *)(reinterpret_cast<void *>(widget->winId()));
    NSWindow *window = view.window;
    if (!window)
        return;
    window.collectionBehavior =
        (window.collectionBehavior & ~NSWindowCollectionBehaviorCanJoinAllSpaces) |
        NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.level = NSFloatingWindowLevel;
}
AutostartState autostartState() {
    AutostartState state;
    NSBundle *bundle = NSBundle.mainBundle;
    if (![bundle.bundleURL.pathExtension isEqualToString:@"app"] ||
        ![bundle.bundleIdentifier isEqualToString:@"io.github.ranslator.Ranslator"]) {
        state.supported = false;
        state.detail = "Open the packaged Ranslator.app to configure login startup. "
                       "A development executable cannot register as a login item.";
        return state;
    }
    if (@available(macOS 13.0, *)) {
        switch (SMAppService.mainAppService.status) {
        case SMAppServiceStatusEnabled:
            state.enabled = true;
            state.detail = "On. macOS launches Ranslator when you log in.";
            break;
        case SMAppServiceStatusRequiresApproval:
            state.requiresApproval = true;
            state.detail = "Approval is needed in System Settings → General → Login Items. "
                           "Allow Ranslator there, then return here.";
            break;
        case SMAppServiceStatusNotFound:
            state.supported = false;
            state.detail = "macOS could not find the login item. Move Ranslator.app to "
                           "Applications, reopen it and try again.";
            break;
        case SMAppServiceStatusNotRegistered:
            state.detail = "Off. Ranslator is not registered as a login item.";
            break;
        }
    } else {
        state.supported = false;
        state.detail = "Login startup requires macOS 13 or later.";
    }
    return state;
}
void setAutostart(QObject *owner, bool enabled, std::function<void(AutostartState, QString)> done) {
    const auto state = autostartState();
    if (!state.supported) {
        done(state, state.detail);
        return;
    }
    if (@available(macOS 13.0, *)) {
        QPointer<QObject> guard(owner);
        // ServiceManagement can contact system services; keep that work off the UI thread.
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
          NSError *error = nil;
          if (enabled)
              [SMAppService.mainAppService registerAndReturnError:&error];
          else
              [SMAppService.mainAppService unregisterAndReturnError:&error];
          const auto message =
              error ? QString::fromNSString(error.localizedDescription) : QString();
          dispatch_async(dispatch_get_main_queue(), ^{
            if (!guard)
                return;
            const auto actual = autostartState();
            // Already-registered and revoked-consent errors are resolved by the OS status.
            const bool succeeded = actual.enabled == enabled && !actual.requiresApproval;
            done(actual, succeeded || actual.requiresApproval
                             ? QString()
                             : "macOS could not change login startup. " + message +
                                   " Review Ranslator in Login Items settings.");
          });
        });
    }
}
void openLoginItemsSettings() {
    if (@available(macOS 13.0, *))
        [SMAppService openSystemSettingsLoginItems];
}
} // namespace NativeMac
