#include "x11.hpp"
#include <QGuiApplication>
#include <QKeySequence>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#ifdef HAVE_X11_SHORTCUTS
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xinput.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#endif

struct X11Shortcuts::State {
    X11Shortcuts *owner;
    QJsonObject config;
    bool modifiersOnly = false;
    std::function<void(QString)> triggered = [](QString) {}, error = [](QString) {};
#ifdef HAVE_X11_SHORTCUTS
    xcb_connection_t *connection = nullptr;
    xcb_key_symbols_t *symbols = nullptr;
    QList<xcb_window_t> roots;
    QSocketNotifier *notifier = nullptr;
    struct Binding {
        xcb_keycode_t code;
        uint16_t modifiers;
        QString id;
    };
    QList<Binding> bindings;
    QHash<unsigned, QString> modifierBindings;
    QHash<xcb_keycode_t, unsigned> downModifiers;
    QSet<xcb_keycode_t> heldShortcuts, downOrdinary;
    QHash<xcb_keycode_t, xcb_timestamp_t> coreReleases;
    QList<uint16_t> lockCombinations;
    QHash<unsigned, uint16_t> modifierMasks;
    uint16_t ignoredLocks = XCB_MOD_MASK_LOCK;
    uint8_t xiOpcode = 0;
    unsigned peak = 0;
    bool used = false, gestureActive = false;
    xcb_timestamp_t gestureStarted = 0;

    static unsigned modifier(xcb_keysym_t symbol) {
        switch (symbol) {
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
            return 1;
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
            return 2;
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:
            return 4;
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R:
            return 8;
        default:
            return 0;
        }
    }
    static unsigned modifierChord(const QString &sequence) {
        unsigned mask = 0;
        for (const auto &part : sequence.split('+')) {
            const auto key = part.trimmed().toLower();
            if (key == "ctrl" || key == "control")
                mask |= 1;
            else if (key == "alt")
                mask |= 2;
            else if (key == "shift")
                mask |= 4;
            else if (key == "meta" || key == "super")
                mask |= 8;
            else
                return 0;
        }
        return mask;
    }
    static xcb_keysym_t keysym(Qt::Key key) {
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
            return XKB_KEY_F1 + key - Qt::Key_F1;
        switch (key) {
        case Qt::Key_Escape:
            return XKB_KEY_Escape;
        case Qt::Key_Tab:
            return XKB_KEY_Tab;
        case Qt::Key_Backtab:
            return XKB_KEY_ISO_Left_Tab;
        case Qt::Key_Backspace:
            return XKB_KEY_BackSpace;
        case Qt::Key_Return:
            return XKB_KEY_Return;
        case Qt::Key_Enter:
            return XKB_KEY_KP_Enter;
        case Qt::Key_Insert:
            return XKB_KEY_Insert;
        case Qt::Key_Delete:
            return XKB_KEY_Delete;
        case Qt::Key_Pause:
            return XKB_KEY_Pause;
        case Qt::Key_Print:
            return XKB_KEY_Print;
        case Qt::Key_Home:
            return XKB_KEY_Home;
        case Qt::Key_End:
            return XKB_KEY_End;
        case Qt::Key_Left:
            return XKB_KEY_Left;
        case Qt::Key_Up:
            return XKB_KEY_Up;
        case Qt::Key_Right:
            return XKB_KEY_Right;
        case Qt::Key_Down:
            return XKB_KEY_Down;
        case Qt::Key_PageUp:
            return XKB_KEY_Prior;
        case Qt::Key_PageDown:
            return XKB_KEY_Next;
        case Qt::Key_Space:
            return XKB_KEY_space;
        default:
            if (uint(key) < 0x01000000)
                return xkb_utf32_to_keysym(QChar(uint(key)).toLower().unicode());
            return xkb_keysym_from_name(QKeySequence(key).toString().toLatin1().constData(),
                                        XKB_KEYSYM_CASE_INSENSITIVE);
        }
    }
    void unbind() {
        for (auto root : roots)
            xcb_ungrab_key(connection, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
        bindings.clear();
        modifierBindings.clear();
        heldShortcuts.clear();
        coreReleases.clear();
        downModifiers.clear();
        downOrdinary.clear();
        gestureActive = false;
        xcb_flush(connection);
    }
    void initializeLocks() {
        ignoredLocks = XCB_MOD_MASK_LOCK;
        modifierMasks = {{1, XCB_MOD_MASK_CONTROL},
                         {2, XCB_MOD_MASK_1},
                         {4, XCB_MOD_MASK_SHIFT},
                         {8, XCB_MOD_MASK_4}};
        auto *reply = xcb_get_modifier_mapping_reply(connection,
                                                     xcb_get_modifier_mapping(connection), nullptr);
        if (reply) {
            modifierMasks.clear();
            const auto *codes = xcb_get_modifier_mapping_keycodes(reply);
            for (unsigned mod = 0; mod < 8; ++mod)
                for (unsigned index = 0; index < reply->keycodes_per_modifier; ++index) {
                    auto symbol = xcb_key_symbols_get_keysym(
                        symbols, codes[mod * reply->keycodes_per_modifier + index], 0);
                    if (symbol == XKB_KEY_Num_Lock || symbol == XKB_KEY_Scroll_Lock)
                        ignoredLocks |= 1u << mod;
                    if (auto bit = modifier(symbol))
                        modifierMasks[bit] |= 1u << mod;
                }
            free(reply);
        }
        lockCombinations = {0};
        for (unsigned bit = 1; bit < 256; bit <<= 1)
            if (ignoredLocks & bit) {
                const auto previous = lockCombinations;
                for (auto value : previous)
                    lockCombinations.append(value | bit);
            }
    }
    void bind() {
        unbind();
        initializeLocks();
        for (const auto &id : {"once", "continuous", "snapshot"}) {
            auto sequence = config[id].toObject()["sequence"].toString();
            if (const auto chord = modifierChord(sequence)) {
                if (xiOpcode)
                    modifierBindings.insert(chord, id);
                else
                    error("Modifier-only shortcuts require XInput 2 on this X11 server.");
                continue;
            }
            if (modifiersOnly)
                continue;
            const auto key = QKeySequence::fromString(sequence, QKeySequence::PortableText);
            if (key.count() != 1 || key[0].key() == Qt::Key_unknown) {
                error("Invalid shortcut: " + sequence);
                continue;
            }
            const auto qtmods = key[0].keyboardModifiers();
            const uint16_t mods =
                (qtmods.testFlag(Qt::ControlModifier) ? modifierMasks.value(1) : 0) |
                (qtmods.testFlag(Qt::AltModifier) ? modifierMasks.value(2) : 0) |
                (qtmods.testFlag(Qt::ShiftModifier) ? modifierMasks.value(4) : 0) |
                (qtmods.testFlag(Qt::MetaModifier) ? modifierMasks.value(8) : 0);
            auto *codes = xcb_key_symbols_get_keycode(symbols, keysym(key[0].key()));
            if (!codes || !codes[0]) {
                free(codes);
                error("Shortcut " + sequence + " is unavailable in the current keyboard map.");
                continue;
            }
            bool failed = false;
            for (auto *code = codes; *code; ++code)
                for (auto root : roots)
                    for (auto locks : lockCombinations) {
                        auto *problem = xcb_request_check(
                            connection,
                            xcb_grab_key_checked(connection, false, root, mods | locks, *code,
                                                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
                        if (problem) {
                            failed = true;
                            free(problem);
                        }
                    }
            if (failed) {
                for (auto *code = codes; *code; ++code)
                    for (auto root : roots)
                        for (auto locks : lockCombinations)
                            xcb_ungrab_key(connection, *code, root, mods | locks);
                error("Shortcut " + sequence + " is already used by another application.");
            } else {
                for (auto *code = codes; *code; ++code)
                    bindings.append({*code, mods, id});
            }
            free(codes);
        }
        xcb_flush(connection);
    }
    void rawKey(const xcb_input_raw_key_press_event_t *event, bool pressed) {
        if (event->flags & XCB_INPUT_KEY_EVENT_FLAGS_KEY_REPEAT)
            return;
        const auto code = xcb_keycode_t(event->detail);
        const auto bit = modifier(xcb_key_symbols_get_keysym(symbols, code, 0));
        if (!pressed)
            heldShortcuts.remove(code);
        if (!bit) {
            if (pressed) {
                downOrdinary.insert(code);
                used = true;
            } else
                downOrdinary.remove(code);
            return;
        }
        if (pressed) {
            if (downModifiers.isEmpty()) {
                peak = 0;
                used = !downOrdinary.isEmpty();
                gestureStarted = event->time;
                gestureActive = true;
            }
            downModifiers.insert(code, bit);
            peak |= bit;
        } else {
            downModifiers.remove(code);
            if (downModifiers.isEmpty() && gestureActive) {
                // Use server timestamps so a busy UI cannot turn a long hold into a tap.
                if (!used && xcb_timestamp_t(event->time - gestureStarted) <= 400 &&
                    modifierBindings.contains(peak))
                    triggered(modifierBindings[peak]);
                gestureActive = false;
            }
        }
    }
    void readEvents() {
        while (auto *event = xcb_poll_for_event(connection)) {
            const auto type = event->response_type & 0x7f;
            if (type == XCB_KEY_PRESS) {
                const auto *key = reinterpret_cast<xcb_key_press_event_t *>(event);
                // Without detectable autorepeat, X11 sends a synthetic release/press
                // pair with the same key and timestamp. Retain suppression across that
                // pair, even when XI2 raw events are absent during a passive key grab.
                const bool repeat = coreReleases.contains(key->detail) &&
                                    coreReleases.take(key->detail) == key->time;
                if (repeat) {
                    heldShortcuts.insert(key->detail);
                } else if (!heldShortcuts.contains(key->detail)) {
                    for (const auto &binding : bindings)
                        if (binding.code == key->detail &&
                            binding.modifiers == (key->state & 0xff & ~ignoredLocks)) {
                            heldShortcuts.insert(key->detail);
                            triggered(binding.id);
                            break;
                        }
                }
            } else if (type == XCB_KEY_RELEASE) {
                const auto *key = reinterpret_cast<xcb_key_release_event_t *>(event);
                coreReleases.insert(key->detail, key->time);
                heldShortcuts.remove(key->detail);
            } else if (type == XCB_GE_GENERIC && xiOpcode) {
                auto *raw = reinterpret_cast<xcb_input_raw_key_press_event_t *>(event);
                if (raw->extension == xiOpcode && (raw->event_type == XCB_INPUT_RAW_KEY_PRESS ||
                                                   raw->event_type == XCB_INPUT_RAW_KEY_RELEASE))
                    rawKey(raw, raw->event_type == XCB_INPUT_RAW_KEY_PRESS);
            } else if (type == XCB_MAPPING_NOTIFY) {
                xcb_refresh_keyboard_mapping(symbols,
                                             reinterpret_cast<xcb_mapping_notify_event_t *>(event));
                bind();
            }
            free(event);
        }
        if (xcb_connection_has_error(connection)) {
            notifier->setEnabled(false);
            error(
                "The X11 connection closed; restart Ranslator after reconnecting to the desktop.");
        }
    }
#endif
    explicit State(X11Shortcuts *parent) : owner(parent) {
#ifdef HAVE_X11_SHORTCUTS
        // An XWayland root only contains X11 surfaces, so it cannot provide desktop-wide parity.
        if (QGuiApplication::platformName() != "xcb" ||
            qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland")
            return;
        connection = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(connection)) {
            xcb_disconnect(connection);
            connection = nullptr;
            return;
        }
        symbols = xcb_key_symbols_alloc(connection);
        for (auto it = xcb_setup_roots_iterator(xcb_get_setup(connection)); it.rem;
             xcb_screen_next(&it))
            roots.append(it.data->root);
        const auto *extension = xcb_get_extension_data(connection, &xcb_input_id);
        if (extension && extension->present) {
            auto *version = xcb_input_xi_query_version_reply(
                connection, xcb_input_xi_query_version(connection, 2, 0), nullptr);
            if (version && version->major_version >= 2) {
                struct {
                    xcb_input_event_mask_t header;
                    uint32_t bits;
                } mask = {{XCB_INPUT_DEVICE_ALL_MASTER, 1},
                          XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS |
                              XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE};
                bool okay = true;
                for (auto root : roots) {
                    auto *problem = xcb_request_check(
                        connection,
                        xcb_input_xi_select_events_checked(connection, root, 1, &mask.header));
                    if (problem) {
                        okay = false;
                        free(problem);
                    }
                }
                if (okay)
                    xiOpcode = extension->major_opcode;
            }
            free(version);
        }
        notifier =
            new QSocketNotifier(xcb_get_file_descriptor(connection), QSocketNotifier::Read, owner);
        QObject::connect(notifier, &QSocketNotifier::activated, owner, [this] { readEvents(); });
        xcb_flush(connection);
#endif
    }
    ~State() {
#ifdef HAVE_X11_SHORTCUTS
        delete notifier;
        if (symbols)
            xcb_key_symbols_free(symbols);
        if (connection)
            xcb_disconnect(connection);
#endif
    }
};
X11Shortcuts::X11Shortcuts(QObject *parent)
    : QObject(parent), state(std::make_unique<State>(this)) {}
X11Shortcuts::~X11Shortcuts() = default;
bool X11Shortcuts::available() const {
#ifdef HAVE_X11_SHORTCUTS
    return state->connection && !xcb_connection_has_error(state->connection);
#else
    return false;
#endif
}
bool X11Shortcuts::retainsInputFocus(quintptr window) {
#ifdef HAVE_X11_SHORTCUTS
    if (!window || QGuiApplication::platformName() != "xcb" ||
        qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland")
        return false;
    auto *connection = xcb_connect(nullptr, nullptr);
    bool retained = false;
    if (!xcb_connection_has_error(connection)) {
        auto *focus =
            xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
        auto current = focus ? focus->focus : XCB_WINDOW_NONE;
        free(focus);
        // Native child widgets can own focus inside the recorder's top-level window.
        for (int depth = 0; current > XCB_INPUT_FOCUS_POINTER_ROOT && depth < 32; ++depth) {
            if (current == window) {
                retained = true;
                break;
            }
            auto *tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, current),
                                              nullptr);
            current = tree ? tree->parent : XCB_WINDOW_NONE;
            free(tree);
        }
    }
    xcb_disconnect(connection);
    return retained;
#else
    Q_UNUSED(window);
    return false;
#endif
}
void X11Shortcuts::bind(const QJsonObject &config, std::function<void(QString)> triggered,
                        std::function<void(QString)> error, bool modifiersOnly) {
    state->config = config;
    state->modifiersOnly = modifiersOnly;
    state->triggered = std::move(triggered);
    state->error = std::move(error);
#ifdef HAVE_X11_SHORTCUTS
    if (available()) {
        state->bind();
        return;
    }
#endif
    state->error("Native X11 shortcuts are unavailable in this session.");
}
