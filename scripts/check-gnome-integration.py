#!/usr/bin/python3
"""Run only inside the disposable GNOME lab container; never use the host session."""
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

if os.environ.get('RANSLATOR_GNOME_LAB') != '1':
    raise SystemExit('Run inside the disposable GNOME container with RANSLATOR_GNOME_LAB=1.')
if not (Path('/run/.containerenv').exists() or Path('/.dockerenv').exists()):
    raise SystemExit('This check must run inside an isolated container, not the host desktop.')
if '--session' not in sys.argv:
    raise SystemExit(subprocess.call(['dbus-run-session', '--', sys.executable, __file__, '--session']))

ROOT = Path(__file__).resolve().parent.parent
SERVICE = 'io.github.ranslator.Ranslator.Desktop'
CLIENT = 'io.github.ranslator.Ranslator.Integration'
PATH = '/io/github/ranslator/Ranslator/Desktop'
IFACE = 'io.github.ranslator.Desktop1'
processes = []
with tempfile.TemporaryDirectory(prefix='ranslator-gnome-lab-') as directory:
    tmp = Path(directory)
    for name in ('runtime', 'config', 'data', 'cache', 'home'):
        (tmp / name).mkdir(mode=0o700)
    os.environ.update({
        'HOME': str(tmp / 'home'), 'XDG_RUNTIME_DIR': str(tmp / 'runtime'),
        'XDG_CONFIG_HOME': str(tmp / 'config'), 'XDG_DATA_HOME': str(tmp / 'data'),
        'XDG_CACHE_HOME': str(tmp / 'cache'), 'XDG_CURRENT_DESKTOP': 'GNOME',
        'XDG_SESSION_TYPE': 'wayland', 'GNOME_SHELL_SESSION_MODE': 'user',
        'LIBGL_ALWAYS_SOFTWARE': '1', 'GDK_BACKEND': 'x11',
        'MUTTER_DEBUG_NUM_DUMMY_MONITORS': '2', 'MUTTER_DEBUG_DUMMY_MODE_SPECS': '800x600',
    })
    for key in ('WAYLAND_DISPLAY', 'AT_SPI_BUS_ADDRESS', 'QT_QPA_PLATFORMTHEME'):
        os.environ.pop(key, None)
    extension = tmp / 'data/gnome-shell/extensions/ranslator@ranslator.local'
    shutil.copytree(ROOT / 'integrations/gnome/ranslator@ranslator.local', extension)
    try:
        read_fd, write_fd = os.pipe()
        display = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                                    '1800x900x24', '-nolisten', 'tcp'], pass_fds=(write_fd,))
        processes.append(display)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            os.environ['DISPLAY'] = ':' + pipe.readline().strip()
        subprocess.run(['dbus-update-activation-environment', 'DISPLAY', 'HOME', 'XDG_RUNTIME_DIR',
                        'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_CURRENT_DESKTOP',
                        'XDG_SESSION_TYPE'], check=True)
        subprocess.run(['gsettings', 'set', 'org.gnome.shell', 'enabled-extensions',
                        "['ranslator@ranslator.local']"], check=True)
        subprocess.run(['gsettings', 'set', 'org.gnome.shell', 'disable-user-extensions', 'false'], check=True)
        shell_log = open(ROOT / 'build/gnome-shell-test.log', 'w')
        shell = subprocess.Popen(['gnome-shell', '--nested', '--wayland', '--no-x11',
                                  '--wayland-display=ranslator-test', '--sm-disable'],
                                 stdout=shell_log, stderr=subprocess.STDOUT)
        processes.append(shell)

        import gi
        gi.require_version('Gio', '2.0')
        from gi.repository import Gio, GLib
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        def pump(seconds=.05):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                while GLib.MainContext.default().iteration(False):
                    pass
                time.sleep(.005)
        def wait(predicate, timeout=20):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                if shell.poll() is not None:
                    raise AssertionError(f'GNOME Shell exited: {shell.returncode}; see build/gnome-shell-test.log')
                if predicate(): return
                pump()
            raise AssertionError('GNOME condition timed out; see build/gnome-shell-test.log')
        def owner_exists():
            return bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'NameHasOwner', GLib.Variant('(s)', [SERVICE]),
                None, Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
        wait(owner_exists, 30)
        assert bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'RequestName', GLib.Variant('(su)', [CLIENT, 0]),
            None, Gio.DBusCallFlags.NONE, 1000, None).unpack()[0] == 1
        def call(method, arguments=None, timeout=10000):
            return bus.call_sync(SERVICE, PATH, IFACE, method, arguments, None,
                                 Gio.DBusCallFlags.NONE, timeout, None).unpack()
        config = {key: {'sequence': value} for key, value in
                  [('once', 'Ctrl+Alt+P'), ('continuous', 'Ctrl+Alt+Y'), ('snapshot', 'Ctrl+Alt+O')]}
        unsupported = dict(config, once={'sequence': 'Shift'})
        rejected = json.loads(call('Configure', GLib.Variant('(s)', [json.dumps(unsupported)]))[0])
        assert len(rejected['errors']) == 1 and 'non-modifier key' in rejected['errors'][0], rejected
        result = json.loads(call('Configure', GLib.Variant('(s)', [json.dumps(config)]))[0])
        assert result['errors'] == [], result
        context = json.loads(call('GetContext')[0])
        assert context['width'] > 100 and context['height'] > 100, context
        events = []
        bus.signal_subscribe(SERVICE, IFACE, None, PATH, None, Gio.DBusSignalFlags.NONE,
                             lambda _bus, _sender, _path, _interface, signal, args:
                             events.append((signal, args.unpack())))
        # A second connection cannot invoke privileged capture methods.
        unauthorized = subprocess.run(['gdbus', 'call', '--session', '--dest', SERVICE,
            '--object-path', PATH, '--method', IFACE + '.GetContext'], capture_output=True, text=True)
        assert unauthorized.returncode != 0 and 'not authorized' in unauthorized.stderr, unauthorized

        os.environ.update({'WAYLAND_DISPLAY': 'ranslator-test', 'GDK_BACKEND': 'wayland'})
        fixture_source = '''import gi,sys
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk,Gdk,GLib
app=Gtk.Application(application_id="io.github.ranslator.GnomeFixture")
def activate(app):
    win=Gtk.ApplicationWindow(application=app,title="Ranslator GNOME fixture")
    box=Gtk.Box(orientation=Gtk.Orientation.VERTICAL,spacing=20)
    box.set_margin_top(40);box.set_margin_start(40);box.set_margin_end(40);box.set_margin_bottom(40)
    box.append(Gtk.Label(label="Ranslator isolated GNOME fixture"))
    entry=Gtk.Entry();entry.set_text("Synthetic selection");box.append(entry)
    win.set_child(box);win.set_default_size(700,400);win.present();entry.grab_focus()
    def command(_fd,_condition):
        line=sys.stdin.readline().strip()
        if line=="select":
            Gdk.Display.get_default().get_primary_clipboard().set("Synthetic selection")
        elif line=="text":
            print(entry.get_text(),flush=True)
        return True
    GLib.io_add_watch(sys.stdin,GLib.IO_IN,command)
app.connect("activate",activate);app.run(None)
'''
        fixture = subprocess.Popen([sys.executable, '-c', fixture_source], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True)
        processes.append(fixture)
        pump(2)
        # The nested compositor's parent window receives real XTEST keyboard events.
        tree = subprocess.check_output(['xwininfo', '-root', '-tree'], text=True)
        windows = re.findall(r'(0x[0-9a-f]+) "gnome-shell"', tree)
        assert windows, 'Missing nested compositor window'
        subprocess.run(['xdotool', 'windowmap', windows[-1]], check=True)
        subprocess.run(['xdotool', 'windowfocus', windows[-1]], check=True)
        def key(sequence):
            subprocess.run(['xdotool', 'key', '--clearmodifiers', sequence], check=True)
            pump(.2)
        key('Escape')
        subprocess.run(['xdotool', 'mousemove', '--window', windows[-1], '400', '300', 'click', '1'], check=True)
        pump(.5)
        key('ctrl+alt+p')
        wait(lambda: any(name == 'ShortcutActivated' and args[0] == 'once' for name, args in events), 3)
        events.clear()
        key('shift+a')
        assert not any(name == 'ShortcutActivated' for name, _args in events), events
        fixture.stdin.write('text\n'); fixture.stdin.flush()
        typed = fixture.stdout.readline().strip()
        assert 'A' in typed, f'Keyboard did not reach the native Wayland fixture: {typed!r}'
        key('ctrl+alt+o')
        wait(lambda: any(name == 'ShortcutActivated' and args[0] == 'snapshot' for name, args in events), 3)
        events.clear()
        call('ArmSelection', GLib.Variant('(bbu)', [True, False, 1]))
        # Saving shortcut settings must not silently disarm a continuous selection listener.
        call('Configure', GLib.Variant('(s)', [json.dumps(config)]))
        fixture.stdin.write('select\n'); fixture.stdin.flush()
        try:
            wait(lambda: ('SelectionChanged', (1, 'Synthetic selection')) in events, 3)
        except AssertionError:
            print(events, flush=True); raise
        call('ArmSelection', GLib.Variant('(bbu)', [False, False, 2]))
        subprocess.run(['xdotool', 'mousemove', '--window', windows[-1], '1200', '300'], check=True)
        pump(.1)
        context = json.loads(call('GetContext')[0])
        assert context['index'] == 1 and context['x'] == 800, context
        key('ctrl+alt+o')
        assert any(name == 'ShortcutActivated' and args[0] == 'snapshot' and
                   json.loads(args[1])['index'] == 1 for name, args in events), events
        for mode in ('auto', 'desktop'):
            png, raw_context = call('Capture', GLib.Variant('(si)', [mode, context['index']]))
            png = bytes(png)
            assert png[:8] == b'\x89PNG\r\n\x1a\n', png[:16]
            width, height = struct.unpack('>II', png[16:24])
            assert (width, height) == ((800, 600) if mode == 'auto' else (1600, 600)), (mode, width, height)
            (ROOT / f'build/gnome-{mode}-fixture.png').write_bytes(png)
        print('PASS: GNOME 46 extension authorization, global chords in a native Wayland client, PRIMARY selection, second-monitor targeting and snapshots')
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
