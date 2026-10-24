#pragma once
#include "design.hpp"
#include "i18n.hpp"
#include "x11.hpp"
#include <functional>

// Saved shortcuts keep Qt's portable text ("Alt+Shift+C"). macOS shows them the way the system
// does, with modifier symbols in Apple's order: ⌃⌥⇧⌘ and then the key ("⌥⇧C").
inline QString shortcutLabel(const QString &sequence) {
#ifdef Q_OS_MACOS
    if (sequence.trimmed().isEmpty())
        return sequence;
    QStringList parts = sequence.split('+');
    if (sequence.endsWith("++")) {
        parts.removeLast();
        parts.last() = "+";
    }
    static const QList<QPair<QStringList, QString>> modifiers{{{"Ctrl", "Control"}, "⌃"},
                                                              {{"Alt", "Option"}, "⌥"},
                                                              {{"Shift"}, "⇧"},
                                                              {{"Meta", "Super", "Cmd"}, "⌘"}};
    static const QHash<QString, QString> keys{
        {"Return", "↩"}, {"Enter", "⌅"},  {"Tab", "⇥"},   {"Backspace", "⌫"}, {"Del", "⌦"},
        {"Esc", "⎋"},    {"Left", "←"},   {"Right", "→"}, {"Up", "↑"},        {"Down", "↓"},
        {"PgUp", "⇞"},   {"PgDown", "⇟"}, {"Home", "↖"},  {"End", "↘"}};
    QString label;
    for (const auto &[names, symbol] : modifiers)
        for (const auto &part : parts)
            if (names.contains(part.trimmed(), Qt::CaseInsensitive)) {
                label += symbol;
                break;
            }
    for (const auto &part : parts) {
        const auto key = part.trimmed();
        bool modifier = false;
        for (const auto &[names, symbol] : modifiers)
            modifier |= names.contains(key, Qt::CaseInsensitive);
        if (!modifier)
            label += keys.value(key, key);
    }
    return label;
#else
    return sequence;
#endif
}

// One chord, including modifier-only gestures. Repetition is configured separately.
// The saved sequence never contains the temporary recording instructions.
class ShortcutRecorder final : public QPushButton {
    QString saved, candidate, hint, instructions;
    QHash<quint64, int> held;
    Qt::KeyboardModifiers peak;
    int ordinaryKey = 0;
    bool listening = false, invalid = false, registered = false;
    QTimer registeredRelease;
    QElapsedTimer registeredStarted;
    bool registeredSawModifiers = false, registeredNeedsRelease = false;
    std::function<void(bool)> stateChanged;
    inline static QPointer<ShortcutRecorder> current;
    inline static QElapsedTimer finished;

    static Qt::KeyboardModifier modifier(int key) {
        switch (key) {
        case Qt::Key_Control:
            return Qt::ControlModifier;
        case Qt::Key_Alt:
            return Qt::AltModifier;
        case Qt::Key_Shift:
            return Qt::ShiftModifier;
        case Qt::Key_Meta:
            return Qt::MetaModifier;
        default:
            return Qt::NoModifier;
        }
    }
    static quint64 identity(QKeyEvent *event) {
        return event->nativeScanCode()
                   ? (quint64(1) << 32) | event->nativeScanCode()
                   : quint64(event->key() == Qt::Key_Backtab ? Qt::Key_Tab : event->key());
    }
    void updateCandidate() {
        if (ordinaryKey) {
            candidate = QKeySequence(QKeyCombination(peak, Qt::Key(ordinaryKey)))
                            .toString(QKeySequence::PortableText);
        } else {
            QStringList parts;
            if (peak.testFlag(Qt::ControlModifier))
                parts << "Ctrl";
            if (peak.testFlag(Qt::AltModifier))
                parts << "Alt";
            if (peak.testFlag(Qt::ShiftModifier))
                parts << "Shift";
            if (peak.testFlag(Qt::MetaModifier))
                parts << "Meta";
            candidate = parts.join('+');
        }
        setText(candidate.isEmpty() ? UiText::tr("Listening… Press keys")
                                    : shortcutLabel(candidate));
        update();
    }
    void end(bool accept) {
        if (!listening)
            return;
        if (accept && !candidate.isEmpty())
            saved = candidate;
        listening = false;
        registered = false;
        registeredRelease.stop();
        setToolTip(instructions);
        current.clear();
        held.clear();
        finished.start();
        setText(label());
        setAccessibleDescription(
            UiText::tr("Activate to record a shortcut. Escape cancels recording."));
        if (stateChanged)
            stateChanged(false);
        update();
    }
    void finishRegisteredIfReleased() {
        if (!listening || !registered)
            return;
        const auto modifiers =
            QGuiApplication::queryKeyboardModifiers() &
            (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
        if (modifiers) {
            registeredSawModifiers = true;
            return;
        }
        // Wayland can deliver D-Bus activation before its queued modifier state. A zero on
        // the first query is not proof of release. Outlast KWin's maximum 400ms modifier tap
        // if this backend never exposes the held state; ordinary unmodified keys need no wait.
        if (!registeredNeedsRelease || registeredSawModifiers || registeredStarted.elapsed() >= 425)
            end(true);
    }
    void press(QKeyEvent *event) {
        if (event->isAutoRepeat())
            return;
        if (event->key() == Qt::Key_Escape) {
            end(false);
            return;
        }
        if (registered)
            return; // Retain the exact native chord while its physical modifiers are held.
        if (held.isEmpty() && !invalid)
            hint.clear();
        const int key = event->key() == Qt::Key_Backtab ? Qt::Key_Tab : event->key();
        held.insert(identity(event), key);
        if (!key || key == Qt::Key_unknown || key == Qt::Key_AltGr) {
            invalid = true;
            hint = UiText::tr("Unsupported key. Release and try another chord.");
            update();
            return;
        }
        peak |= event->modifiers() &
                (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
        if (event->key() == Qt::Key_Backtab)
            peak |= Qt::ShiftModifier;
        const auto bit = modifier(key);
        if (bit != Qt::NoModifier) {
            peak |= bit;
        } else if (ordinaryKey && ordinaryKey != key) {
            invalid = true;
            hint = UiText::tr("Use one key with optional modifiers. Release to retry.");
        } else {
            ordinaryKey = key;
            peak |= event->modifiers() & Qt::KeypadModifier;
        }
        updateCandidate();
    }
    void release(QKeyEvent *event) {
        if (event->isAutoRepeat())
            return;
        if (registered) {
            finishRegisteredIfReleased();
            return;
        }
        // Ignore the release of Enter/Space used to start recording.
        if (!held.remove(identity(event)))
            return;
        if (!held.isEmpty())
            return;
        if (invalid) {
            invalid = false;
            ordinaryKey = 0;
            peak = {};
            candidate.clear();
            updateCandidate();
        } else if (!candidate.isEmpty()) {
            end(true);
        }
    }

  protected:
    bool event(QEvent *event) override {
        if (listening && event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (listening &&
            (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (event->type() == QEvent::KeyPress)
                press(key);
            else
                release(key);
            event->accept();
            return true; // Includes Tab, which would otherwise move focus.
        }
        return QPushButton::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            begin();
            event->accept();
        } else {
            QPushButton::keyPressEvent(event);
        }
    }
    void focusOutEvent(QFocusEvent *event) override {
        if (event->reason() == Qt::ActiveWindowFocusReason)
            cancelOnWindowDeactivation();
        else
            end(false);
        QPushButton::focusOutEvent(event);
    }
    void hideEvent(QHideEvent *event) override {
        end(false);
        QPushButton::hideEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto &theme = Design::theme();
        const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setBrush(listening ? QColor(88, 137, 239, theme.dark ? 35 : 25)
                                   : QColor(255, 255, 255, theme.dark ? 10 : 110));
        painter.setPen(QPen(listening || hasFocus() ? QColor("#729afa")
                                                    : QColor(255, 255, 255, theme.dark ? 26 : 160),
                            listening ? 1.5 : 1));
        painter.drawRoundedRect(bounds, 10, 10);
        painter.setPen(theme.ink());
        auto value = text();
        if (listening && !hint.isEmpty())
            value = hint;
        const QRect content(14, 0, qMax(20, width() - 88), height());
        painter.drawText(content, Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(value, Qt::ElideRight, content.width()));
        auto small = font();
        small.setPixelSize(11);
        painter.setFont(small);
        painter.setPen(listening ? QColor("#729afa") : theme.secondary());
        painter.drawText(QRect(width() - 70, 0, 55, height()), Qt::AlignVCenter | Qt::AlignRight,
                         listening ? "Esc" : UiText::tr("Record"));
    }

  public:
    ShortcutRecorder(QString sequence, std::function<void(bool)> changed, QWidget *parent = nullptr)
        : QPushButton(parent), saved(std::move(sequence)), stateChanged(std::move(changed)) {
        setText(label());
        setAutoDefault(false);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(42);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        instructions = UiText::tr(
            "Click, then press and release a shortcut. Escape cancels. "
            "Set repeated presses separately. System-reserved shortcuts may be unavailable.");
        setToolTip(instructions);
        setAccessibleDescription(
            UiText::tr("Activate to record a shortcut. Escape cancels recording."));
        registeredRelease.setInterval(20);
        connect(&registeredRelease, &QTimer::timeout, this,
                [this] { finishRegisteredIfReleased(); });
        connect(this, &QPushButton::clicked, this, [this] { begin(); });
    }
    ~ShortcutRecorder() override { end(false); }
    QSize sizeHint() const override { return {260, 42}; }
    QSize minimumSizeHint() const override { return {160, 42}; }
    QString sequence() const { return saved; }
    // An empty sequence leaves an optional action without a shortcut.
    void clear() {
        end(false);
        saved.clear();
        setText(label());
    }
    QString label() const { return saved.isEmpty() ? UiText::tr("Not set") : shortcutLabel(saved); }
    bool isRecording() const { return listening; }
    static ShortcutRecorder *active() { return current; }
    static bool justFinished() { return finished.isValid() && finished.elapsed() < 250; }
    void begin() {
        if (listening)
            return;
        if (current)
            current->cancel();
        current = this;
        held.clear();
        peak = {};
        ordinaryKey = 0;
        candidate.clear();
        hint.clear();
        invalid = false;
        listening = true;
        setFocus(Qt::OtherFocusReason);
        updateCandidate();
        setAccessibleDescription(
            UiText::tr("Listening for a shortcut. Press keys, then release. Escape cancels."));
        if (stateChanged)
            stateChanged(true);
    }
    void cancel() { end(false); }
    void cancelOnWindowDeactivation() {
        if (!listening)
            return;
        // X11 passive grabs briefly deactivate Qt. Preserve recording only when the
        // server still focuses this window; actual app switches and click-away cancel.
        if (!X11Shortcuts::retainsInputFocus(window()->winId()))
            end(false);
    }
    void acceptRegisteredShortcut(const QString &sequence) {
        if (!listening)
            return;
        candidate = sequence;
        registered = true;
        registeredSawModifiers = false;
        registeredNeedsRelease = false;
        for (const auto &part : sequence.split('+'))
            registeredNeedsRelease |=
                QStringList{"Ctrl", "Control", "Alt", "Shift", "Meta", "Super"}.contains(
                    part.trimmed(), Qt::CaseInsensitive);
        registeredStarted.start();
        setText(shortcutLabel(candidate));
        setAccessibleDescription(UiText::tr("Release the shortcut keys to finish recording."));
        // X11/Carbon/global services can swallow Qt releases. Query the real modifier state
        // rather than finishing on activation and allowing a late KWin modifier tap through.
        registeredRelease.start();
        finishRegisteredIfReleased();
    }
    void interceptedShortcut() {
        hint = UiText::tr("Desktop intercepted this chord. Try another, or press Esc.");
        setAccessibleDescription(hint);
        setToolTip(hint);
        update();
    }
};
