#!/usr/bin/python3
"""Exercise a packaged app on an isolated Sway compositor, never the host session."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
if os.environ.get('RANSLATOR_WLROOTS_LAB') != '1' or not (
        Path('/run/.containerenv').exists() or Path('/.dockerenv').exists()):
    raise SystemExit('Run in the disposable wlroots container with RANSLATOR_WLROOTS_LAB=1.')
sys.path.insert(0, str(ROOT / 'tests/fixtures'))
from wlroots_virtual_pointer import Pointer
if '--session' not in sys.argv:
    raise SystemExit(subprocess.call(['dbus-run-session', '--', sys.executable, __file__, '--session']))

requests = []
processes = []
class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = body['messages'][-1]['content']
        requests.append(text)
        output = json.dumps({'choices': [{'message': {'content': 'Translated fixture'}}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(output)))
        self.end_headers()
        self.wfile.write(output)

with tempfile.TemporaryDirectory(prefix='ranslator-wlroots-lab-') as directory:
    tmp = Path(directory)
    for name in ('home', 'runtime', 'config', 'data', 'cache', 'tmp', 'bundle'):
        (tmp / name).mkdir(mode=0o700)
    env = os.environ.copy()
    env.update({'HOME': str(tmp / 'home'), 'XDG_RUNTIME_DIR': str(tmp / 'runtime'),
                'XDG_CONFIG_HOME': str(tmp / 'config'), 'XDG_DATA_HOME': str(tmp / 'data'),
                'XDG_CACHE_HOME': str(tmp / 'cache'), 'TMPDIR': str(tmp / 'tmp'),
                'XDG_CURRENT_DESKTOP': 'sway', 'XDG_SESSION_DESKTOP': 'sway',
                'XDG_SESSION_TYPE': 'wayland', 'WLR_BACKENDS': 'headless',
                'WLR_RENDERER': 'pixman', 'WLR_HEADLESS_OUTPUTS': '2',
                'LIBGL_ALWAYS_SOFTWARE': '1', 'QT_QPA_PLATFORM': 'wayland',
                'QT_WAYLAND_CLIENT_BUFFER_INTEGRATION': 'shm', 'GDK_BACKEND': 'wayland',
                'QT_LINUX_ACCESSIBILITY_ALWAYS_ON': '1',
                'RANSLATOR_CONFIG': str(tmp / 'config.json')})
    for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'SWAYSOCK', 'AT_SPI_BUS_ADDRESS',
                 'QT_QPA_PLATFORMTHEME', 'NO_AT_BRIDGE', 'RANSLATOR_API_KEY',
                 'http_proxy', 'https_proxy', 'all_proxy', 'HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY'):
        env.pop(name, None)
    os.environ.clear()
    os.environ.update(env)
    logs = ROOT / 'build/wlroots'
    logs.mkdir(parents=True, exist_ok=True)
    (logs / 'report.json').unlink(missing_ok=True)
    def launch(argv, name, **kwargs):
        process = subprocess.Popen(argv, stdout=open(logs / f'{name}.log', 'w'),
                                   stderr=subprocess.STDOUT, **kwargs)
        processes.append(process)
        return process
    def wait(predicate, timeout=15):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            result = predicate()
            if result:
                return result
            time.sleep(.05)
        raise AssertionError(f'wlroots condition timed out; inspect {logs}')
    def sway(command):
        result = json.loads(subprocess.check_output(['swaymsg', '-t', 'command', command], text=True))
        assert all(entry['success'] for entry in result), result
    def tree_nodes(node=None):
        if node is None:
            node = json.loads(subprocess.check_output(['swaymsg', '-t', 'get_tree'], text=True))
        yield node
        for child in node.get('nodes', []) + node.get('floating_nodes', []):
            yield from tree_nodes(child)
    def copy(text, primary=True):
        subprocess.run(['wl-copy', '--type', 'text/plain;charset=utf-8'] +
                       (['--primary'] if primary else []), input=text, text=True, check=True)
    def focus_fixture():
        sway('[app_id="io.github.ranslator.WlrootsFixture"] focus')
        time.sleep(.15)
    def accessible_nodes(node=None):
        if node is None:
            node = Atspi.get_desktop(0)
        yield node
        try:
            for index in range(node.get_child_count()):
                child = node.get_child_at_index(index)
                if child and child.get_role_name() not in ('list', 'menu'):
                    yield from accessible_nodes(child)
        except GLib.Error:
            return
    def command(action):
        subprocess.run([str(app), action], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=10)
        time.sleep(.25)
    try:
        archive = Path(os.environ.get('RANSLATOR_TEST_BUNDLE',
                                      ROOT / 'dist/portable/Ranslator-linux-x86_64.tar.gz'))
        shutil.copyfile(archive, tmp / 'bundle.tar.gz')
        bundle_sha = hashlib.sha256((tmp / 'bundle.tar.gz').read_bytes()).hexdigest()
        with tarfile.open(tmp / 'bundle.tar.gz') as bundle:
            bundle.extractall(tmp / 'bundle', filter='data')
        app = tmp / 'bundle/AppRun'
        sway_config = tmp / 'sway.conf'
        sway_config.write_text('''xwayland disable
output HEADLESS-1 mode 1200x800 position 0 0
output HEADLESS-2 mode 1200x800 position 1200 0
seat seat0 fallback true
default_border none
focus_follows_mouse no
''')
        compositor = launch(['sway', '--unsupported-gpu', '-c', str(sway_config)], 'sway')
        socket = wait(lambda: next((p for p in (tmp / 'runtime').glob('wayland-*')
                                   if not p.name.endswith('.lock')), None))
        os.environ['WAYLAND_DISPLAY'] = socket.name
        os.environ['SWAYSOCK'] = str(wait(lambda: next((tmp / 'runtime').glob('sway-ipc*'), None)))
        # Keep seat capabilities stable before Qt connects; one-shot input clients disappear
        # before a headless session's clients can bind their newly advertised pointer.
        pointer = Pointer()
        launch(['pipewire'], 'pipewire')
        subprocess.run(['dbus-update-activation-environment', 'WAYLAND_DISPLAY', 'XDG_CURRENT_DESKTOP',
                        'XDG_SESSION_TYPE', 'HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME',
                        'XDG_RUNTIME_DIR', 'GDK_BACKEND'], check=True)
        portal_config = tmp / 'config/xdg-desktop-portal'
        portal_config.mkdir()
        (portal_config / 'sway-portals.conf').write_text('''[preferred]
default=gtk
org.freedesktop.impl.portal.Screenshot=wlr
org.freedesktop.impl.portal.ScreenCast=wlr
''')
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi, GLib
        fixture_source = '''import gi
gi.require_version("Gtk","4.0")
from gi.repository import Gtk,Gdk
app=Gtk.Application(application_id="io.github.ranslator.WlrootsFixture")
def activate(app):
    css=Gtk.CssProvider();css.load_from_data(b"label { font-size: 32px; }")
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(),css,600)
    window=Gtk.ApplicationWindow(application=app,title="Synthetic Wayland fixture")
    label=Gtk.Label(label="SYNTHETIC SNAPSHOT TEXT")
    window.set_child(label);window.set_default_size(800,600);window.present()
app.connect("activate",activate);app.run(None)
'''
        launch([sys.executable, '-c', fixture_source], 'fixture')
        wait(lambda: any(n.get('app_id') == 'io.github.ranslator.WlrootsFixture' for n in tree_nodes()))
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        config = json.loads((ROOT / 'config.example.json').read_text())
        config.update(provider='compatible', endpoint=f'http://127.0.0.1:{server.server_port}/v1',
                      model='fixture', preferred_language='ru', fallback_language='en',
                      selection_delay_ms=250, ocr_languages='eng', appearance='light')
        (tmp / 'config.json').write_text(json.dumps(config))
        copy('Existing selection must be ignored')
        application = launch([str(app), '--window'], 'app')
        time.sleep(1.5)
        assert application.poll() is None, (logs / 'app.log').read_text()
        listen_button = wait(lambda: next((n for n in accessible_nodes()
                                           if n.get_name() == 'Keep listening'), None))
        bounds = Atspi.Component.get_extents(listen_button, Atspi.CoordType.WINDOW)
        app_window = next(n for n in tree_nodes() if n.get('name') == 'Ranslator')
        pointer.move(app_window['rect']['x'] + bounds.x + bounds.width // 2,
                     app_window['rect']['y'] + bounds.y + bounds.height // 2)
        time.sleep(.2)
        pointer.button(True)
        time.sleep(.15)
        pointer.button(False)
        wait(lambda: not any(n.get('name') == 'Ranslator' for n in tree_nodes()))
        time.sleep(.25)
        assert not requests, requests
        focus_fixture()
        copy('First selected phrase')
        wait(lambda: len(requests) == 1)
        assert requests == ['First selected phrase'], requests
        focus_fixture()
        copy('Regular clipboard must be ignored', primary=False)
        time.sleep(.5)
        assert len(requests) == 1, requests
        command('--listen')  # Off.
        copy('Selection while stopped')
        time.sleep(.5)
        assert len(requests) == 1, requests
        command('--listen')  # Rearm; the existing selection must still be ignored.
        focus_fixture()
        time.sleep(.4)
        assert len(requests) == 1, requests
        copy('Second selected phrase')
        wait(lambda: len(requests) == 2)
        assert requests[-1] == 'Second selected phrase', requests
        focus_fixture()
        copy('Cleared before settling')
        subprocess.run(['wl-copy', '--primary', '--clear'], check=True)
        time.sleep(.5)
        assert len(requests) == 2, requests
        copy('Third selected phrase')
        wait(lambda: len(requests) == 3)
        assert requests[-1] == 'Third selected phrase', requests
        wait(lambda: any(n.get_role_name() == 'application' and
                         (n.get_name() or '').lower() == 'ranslator'
                         for n in accessible_nodes()))
        command('--listen')
        focus_fixture()
        command('--snapshot')
        def allow_or_snapshot():
            for node in accessible_nodes():
                name = node.get_name() or ''
                if name.startswith('Screenshot text selection.'):
                    return node
                if node.get_role_name() == 'push button' and name in ('Share', 'Allow'):
                    print('Granting screenshot access only in the disposable session', flush=True)
                    Atspi.Action.do_action(node, 0)
            return None
        try:
            overlay = wait(allow_or_snapshot, 20)
        except AssertionError:
            (logs / 'tree.json').write_text(json.dumps(list(tree_nodes()), indent=2))
            (logs / 'accessibility.txt').write_text('\n'.join(
                f'{n.get_role_name()}: {n.get_name()}' for n in accessible_nodes()))
            subprocess.run(['grim', str(logs / 'failed-screenshot.png')], check=True)
            raise
        wait(lambda: 'SYNTHETIC SNAPSHOT TEXT' in (overlay.get_description() or ''), 20)
        subprocess.run(['grim', str(logs / 'snapshot-overlay.png')], check=True)
        portal = subprocess.check_output([
            'dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.portal.Desktop',
            '/org/freedesktop/portal/desktop', 'org.freedesktop.DBus.Introspectable.Introspect'], text=True)
        assert 'org.freedesktop.portal.GlobalShortcuts' not in portal
        pointer.move(150, 380)
        time.sleep(.2)
        pointer.button(True)
        pointer.move(450, 420)
        time.sleep(.2)
        pointer.button(False)
        wait(lambda: len(requests) == 4, 20)
        assert requests[-1] == 'SYNTHETIC SNAPSHOT TEXT', requests
        pointer.close()
        outputs = json.loads(subprocess.check_output(['swaymsg', '-t', 'get_outputs'], text=True))
        report = {'bundle_sha256': bundle_sha,
                  'compositor': subprocess.check_output(['sway', '--version'], text=True).strip(),
                  'outputs': [item['name'] for item in outputs], 'translations': requests,
                  'qt_accessibility_registered': True,
                  'snapshot_scope': 'whole desktop fitted into one output',
                  'data_control_protocols': [name for name in pointer.globals if 'data_control' in name],
                  'checks': ['real Listen button arms from full translator',
                             'initial PRIMARY ignored', 'PRIMARY translated through packaged app',
                             'regular clipboard excluded', 'disarm/rearm', 'clear cancels pending',
                             'GlobalShortcuts portal absent', 'real screenshot portal and OCR',
                             'Qt accessibility registered', 'pointer-selected OCR translated']}
        (logs / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print('PASS: packaged app on real headless Sway, PRIMARY lifecycle, clipboard filtering, '
              'clear cancellation, Qt accessibility, real screenshot portal, OCR and pointer selection', flush=True)
        command('--quit')
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
