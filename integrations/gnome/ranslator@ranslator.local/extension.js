// SPDX-License-Identifier: MIT
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const SERVICE = 'io.github.ranslator.Ranslator.Desktop';
const CLIENT = 'io.github.ranslator.Ranslator.Integration';
const PATH = '/io/github/ranslator/Ranslator/Desktop';
const IFACE = 'io.github.ranslator.Desktop1';
const XML = `<node><interface name="${IFACE}">
  <method name="Configure"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
  <method name="GetContext"><arg type="s" direction="out"/></method>
  <method name="Capture"><arg type="s" direction="in"/><arg type="i" direction="in"/>
    <arg type="ay" direction="out"/><arg type="s" direction="out"/></method>
  <method name="ArmSelection"><arg type="b" direction="in"/><arg type="b" direction="in"/><arg type="u" direction="in"/></method>
  <signal name="ShortcutActivated"><arg type="s"/><arg type="s"/></signal>
  <signal name="SelectionChanged"><arg type="u"/><arg type="s"/></signal>
</interface></node>`;

function parseSequence(sequence) {
    const parts = sequence.trim().split('+').map(part => part.trim());
    let mask = 0;
    const keys = [];
    for (const part of parts) {
        switch (part.toLowerCase()) {
        case 'ctrl': case 'control': mask |= 1; break;
        case 'alt': mask |= 2; break;
        case 'shift': mask |= 4; break;
        case 'meta': case 'super': mask |= 8; break;
        default: keys.push(part);
        }
    }
    if (keys.length === 0 && mask) return {mask};
    if (keys.length !== 1 || !keys[0]) throw new Error('Use one key chord, for example Ctrl+Alt+R.');
    const aliases = {PgUp: 'Page_Up', PgDown: 'Page_Down', Esc: 'Escape', Del: 'Delete', Ins: 'Insert', Space: 'space'};
    const prefix = [[1, 'Control'], [2, 'Alt'], [4, 'Shift'], [8, 'Super']]
        .filter(([bit]) => mask & bit).map(([, name]) => `<${name}>`).join('');
    return {accelerator: prefix + (aliases[keys[0]] ?? keys[0])};
}

export default class RanslatorIntegration extends Extension {
    enable() {
        this._client = null;
        this._bindings = new Map();
        this._selectionActive = false;
        this._selectionGeneration = 0;
        this._captureBusy = false;
        this._object = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._object.export(Gio.DBus.session, PATH);
        this._name = Gio.bus_own_name_on_connection(Gio.DBus.session, SERVICE,
            Gio.BusNameOwnerFlags.NONE, null, null);
        this._clientWatch = Gio.bus_watch_name_on_connection(Gio.DBus.session, CLIENT,
            Gio.BusNameWatcherFlags.NONE,
            (_connection, _name, owner) => { this._client = owner; },
            () => { this._client = null; this._release(); });
        this._shortcutSignal = global.display.connect('accelerator-activated', (_display, action) => {
            const id = this._bindings.get(action);
            if (id && !this._locked()) this._activate(id);
        });
        this._selectionSignal = global.display.get_selection().connect('owner-changed',
            (_selection, type, owner) => this._selectionChanged(type, owner));
    }
    disable() {
        this._release();
        if (this._shortcutSignal) global.display.disconnect(this._shortcutSignal);
        if (this._selectionSignal) global.display.get_selection().disconnect(this._selectionSignal);
        if (this._clientWatch) Gio.bus_unwatch_name(this._clientWatch);
        this._object?.unexport();
        if (this._name) Gio.bus_unown_name(this._name);
        this._object = null;
        this._client = null;
    }
    _locked() { return Main.sessionMode.isLocked || Main.sessionMode.isGreeter; }
    _releaseBindings() {
        for (const action of this._bindings.keys()) global.display.ungrab_accelerator(action);
        this._bindings.clear();
    }
    _release() {
        this._releaseBindings();
        this._selectionActive = false;
        ++this._selectionGeneration;
    }
    async _authorize(invocation) {
        // Only the running Ranslator client can configure grabs, read selections, or capture.
        // Never claim Orca's identity or alter Mutter access control.
        const reply = await new Promise((resolve, reject) => {
            Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'GetNameOwner', new GLib.Variant('(s)', [CLIENT]),
                new GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE, 1000, null,
                (connection, result) => {
                    try { resolve(connection.call_finish(result).deep_unpack()[0]); }
                    catch (error) { reject(error); }
                });
        });
        if (reply !== invocation.get_sender() || this._locked())
            throw new Error('Ranslator integration is not authorized in this session.');
        this._client = reply;
    }
    _error(invocation, error) {
        invocation.return_dbus_error('io.github.ranslator.Desktop1.Error', error.message ?? String(error));
    }
    async ConfigureAsync([raw], invocation) {
        try {
            await this._authorize(invocation);
            if (raw.length > 4096) throw new Error('Shortcut configuration is too large.');
            const config = JSON.parse(raw);
            this._releaseBindings();
            const errors = [];
            for (const id of ['once', 'continuous', 'snapshot']) {
                try {
                    const sequence = config[id]?.sequence;
                    if (typeof sequence !== 'string') throw new Error('Missing shortcut.');
                    const parsed = parseSequence(sequence);
                    if (parsed.mask) {
                        throw new Error('GNOME requires a shortcut containing a non-modifier key, for example Ctrl+Alt+R.');
                    } else {
                        const action = global.display.grab_accelerator(parsed.accelerator, Meta.KeyBindingFlags.NONE);
                        if (action === Meta.KeyBindingAction.NONE) throw new Error('Shortcut is already in use.');
                        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(action),
                            Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW);
                        this._bindings.set(action, id);
                    }
                } catch (error) { errors.push(`${id}: ${error.message}`); }
            }
            invocation.return_value(new GLib.Variant('(s)', [JSON.stringify({version: 1, errors})]));
        } catch (error) { this._error(invocation, error); }
    }
    _context(index = null) {
        const [cursorX, cursorY] = global.get_pointer();
        if (index === null) {
            // Mutter's current monitor can follow keyboard focus. Snapshot follows the pointer.
            index = Main.layoutManager.monitors.findIndex(monitor => cursorX >= monitor.x &&
                cursorY >= monitor.y && cursorX < monitor.x + monitor.width &&
                cursorY < monitor.y + monitor.height);
            if (index < 0) index = global.display.get_current_monitor();
        }
        const monitor = Main.layoutManager.monitors[index];
        if (!monitor) throw new Error('The selected monitor is no longer connected.');
        return {index, x: monitor.x, y: monitor.y, width: monitor.width, height: monitor.height,
            scale: global.display.get_monitor_scale(index), cursor_x: cursorX, cursor_y: cursorY};
    }
    async GetContextAsync(_params, invocation) {
        try {
            await this._authorize(invocation);
            invocation.return_value(new GLib.Variant('(s)', [JSON.stringify(this._context())]));
        } catch (error) { this._error(invocation, error); }
    }
    _emit(name, variant) {
        if (this._client && this._object)
            Gio.DBus.session.emit_signal(this._client, PATH, IFACE, name, variant);
    }
    _activate(id) {
        this._emit('ShortcutActivated', new GLib.Variant('(ss)', [id, JSON.stringify(this._context())]));
    }
    async ArmSelectionAsync([active, clipboard, generation], invocation) {
        try {
            await this._authorize(invocation);
            ++this._selectionGeneration;
            this._selectionActive = active;
            this._selectionClientGeneration = generation;
            this._watchClipboard = clipboard;
            invocation.return_value(null);
        } catch (error) { this._error(invocation, error); }
    }
    _selectionChanged(type, owner) {
        if (!this._selectionActive || !this._client || this._locked()) return;
        const primary = type === Meta.SelectionType.SELECTION_PRIMARY;
        if (!primary && !(this._watchClipboard && type === Meta.SelectionType.SELECTION_CLIPBOARD)) return;
        const generation = ++this._selectionGeneration;
        if (!owner) {
            this._emit('SelectionChanged', new GLib.Variant('(us)', [this._selectionClientGeneration, '']));
            return;
        }
        St.Clipboard.get_default().get_text(primary ? St.ClipboardType.PRIMARY : St.ClipboardType.CLIPBOARD,
            (_clipboard, text) => {
                if (generation !== this._selectionGeneration || !this._selectionActive || this._locked()) return;
                if (typeof text === 'string' && text.length <= 65536)
                    this._emit('SelectionChanged', new GLib.Variant('(us)', [this._selectionClientGeneration, text]));
            });
    }
    async CaptureAsync([mode, index], invocation) {
        let stream;
        let ownsCapture = false;
        try {
            await this._authorize(invocation);
            if (!['auto', 'desktop'].includes(mode)) throw new Error('Unsupported capture mode.');
            if (this._captureBusy) throw new Error('A Snapshot is already being captured.');
            const context = this._context(index);
            this._captureBusy = true;
            ownsCapture = true;
            const shooter = new Shell.Screenshot();
            stream = Gio.MemoryOutputStream.new_resizable();
            if (mode === 'desktop') await shooter.screenshot(false, stream);
            else await shooter.screenshot_area(context.x, context.y, context.width, context.height, stream);
            stream.close(null);
            const bytes = stream.steal_as_bytes();
            if (bytes.get_size() > 64 * 1024 * 1024) throw new Error('Snapshot exceeds the 64 MiB transfer limit.');
            if (this._locked() || invocation.get_sender() !== this._client)
                throw new Error('Snapshot session ended.');
            invocation.return_value(GLib.Variant.new_tuple([
                GLib.Variant.new_from_bytes(new GLib.VariantType('ay'), bytes, true),
                new GLib.Variant('s', JSON.stringify(context)),
            ]));
        } catch (error) { this._error(invocation, error); }
        finally {
            if (stream && !stream.is_closed()) stream.close(null);
            if (ownsCapture) this._captureBusy = false;
        }
    }
}
