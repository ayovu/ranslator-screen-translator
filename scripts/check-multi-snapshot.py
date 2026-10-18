#!/usr/bin/python3
"""Isolated end-to-end snapshot check: real Qt UI + Rust queue + Tesseract + mock API.
Requires Xvfb, xdotool, Python Pillow/GObject/AT-SPI. Never captures the user's desktop.
"""
import atexit
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

if '--session' not in sys.argv:
    # No desktop-service activation: opening an image does not need global shortcuts or portals.
    with tempfile.TemporaryDirectory(prefix='ranslator-test-bus-') as bus_dir:
        bus_config = Path(bus_dir) / 'session.conf'
        bus_config.write_text('''<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>
<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*" eavesdrop="true"/>
<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>''')
        result = subprocess.call(['/usr/bin/dbus-run-session', '--config-file', str(bus_config),
                                  '--', sys.executable, __file__, '--session'])
    raise SystemExit(result)

ROOT = Path(__file__).resolve().parent.parent
completed = []


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = data['messages'][-1]['content'].strip()
        assert text in ('First region', 'Second region', 'Third region'), text
        time.sleep(1.2 if text == 'First region' else .12)
        translated = {'First region': 'Первый фрагмент', 'Second region': 'Второй фрагмент',
                      'Third region': 'Третий фрагмент'}[text]
        body = json.dumps({'choices': [{'message': {'content': translated}}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        completed.append(text)


with tempfile.TemporaryDirectory(prefix='ranslator-multi-test-') as temporary:
    tmp = Path(temporary)
    for name in ('runtime', 'tmp', 'bin'):
        (tmp / name).mkdir(mode=0o700)
    os.environ.update({'XDG_RUNTIME_DIR': str(tmp / 'runtime'), 'TMPDIR': str(tmp / 'tmp'),
                       'XDG_CONFIG_HOME': str(tmp / 'config'), 'XDG_DATA_HOME': str(tmp / 'data'),
                       'XDG_CACHE_HOME': str(tmp / 'cache'),
                       'QT_QPA_PLATFORM': 'xcb', 'XDG_SESSION_TYPE': 'x11',
                       'XDG_CURRENT_DESKTOP': '', 'QT_LINUX_ACCESSIBILITY_ALWAYS_ON': '1',
                       'RANSLATOR_CONFIG': str(tmp / 'config.json')})
    for name in ('WAYLAND_DISPLAY', 'AT_SPI_BUS_ADDRESS', 'NO_AT_BRIDGE', 'RANSLATOR_API_KEY'):
        os.environ.pop(name, None)
    read_fd, write_fd = os.pipe()
    display = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1200x800x24',
                                '-nolisten', 'tcp'], pass_fds=(write_fd,))
    def stop_display():
        if display.poll() is None:
            display.terminate()
            display.wait(timeout=5)
    atexit.register(stop_display)
    os.close(write_fd)
    with os.fdopen(read_fd) as pipe:
        os.environ['DISPLAY'] = ':' + pipe.readline().strip()
    subprocess.run(['dbus-update-activation-environment', 'DISPLAY', 'QT_QPA_PLATFORM',
                    'XDG_RUNTIME_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME',
                    'XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'WAYLAND_DISPLAY='], check=True)

    accessibility = subprocess.Popen(['/usr/lib/at-spi-bus-launcher', '--launch-immediately'])
    atexit.register(accessibility.terminate)
    for _ in range(30):
        address = subprocess.run(['qdbus6', 'org.a11y.Bus', '/org/a11y/bus',
                                  'org.a11y.Bus.GetAddress'], capture_output=True, text=True)
        if address.returncode == 0:
            os.environ['AT_SPI_BUS_ADDRESS'] = address.stdout.strip()
            break
        time.sleep(.05)
    registry = subprocess.Popen(['/usr/lib/at-spi2-registryd'])
    atexit.register(registry.terminate)

    import gi
    gi.require_version('Atspi', '2.0')
    from gi.repository import Atspi, GLib
    from PIL import Image, ImageDraw, ImageFont, ImageGrab

    scene = Image.new('RGB', (1200, 800), '#f4f5f8')
    painter = ImageDraw.Draw(scene)
    font_path = subprocess.check_output(['fc-match', '-f', '%{file}', 'sans-serif'], text=True)
    font = ImageFont.truetype(font_path, 32)
    for text, y in zip(('First region', 'Second region', 'Third region'), (180, 330, 480)):
        painter.text((90, y), text, font=font, fill='#263448')
    scene.save(tmp / 'scene.png')
    # Delay only full-screen OCR: the three selections should get independent crop jobs.
    wrapper = tmp / 'bin/tesseract'
    wrapper.write_text('#!/usr/bin/python3\nimport os,sys,time\nfrom PIL import Image\n'
                       'if Image.open(sys.argv[1]).width > 1000: time.sleep(2)\n'
                       'os.execv("/usr/bin/tesseract", ["tesseract"] + sys.argv[1:])\n')
    wrapper.chmod(0o755)
    os.environ['PATH'] = str(tmp / 'bin') + ':' + os.environ['PATH']
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    config = json.loads((ROOT / 'config.example.json').read_text())
    config.update(provider='compatible', endpoint=f'http://127.0.0.1:{server.server_port}/v1',
                  model='test', preferred_language='ru', fallback_language='en',
                  ocr_languages='eng', appearance='light', snapshot_copy_text=False)
    (tmp / 'config.json').write_text(json.dumps(config))
    app = subprocess.Popen([str(ROOT / 'target/release/ranslator'), '--image', str(tmp / 'scene.png')],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def nodes(node=None):
        if node is None:
            node = next((a for a in Atspi.get_desktop(0) if a.get_name() == 'ranslator'), None)
            if node is None:
                return
        yield node
        for child in node:
            if child.get_role_name() not in ('list', 'menu'):
                yield from nodes(child)

    def find(name):
        return next(n for n in nodes() if n.get_name() == name and
                    n.get_state_set().contains(Atspi.StateType.SHOWING))

    def wait_for(predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                value = predicate()
                if value:
                    return value
            except (StopIteration, GLib.Error):
                pass
            time.sleep(.05)
        raise AssertionError('Snapshot integration check timed out')

    def mouse(*args):
        subprocess.run(['xdotool', *map(str, args)], check=True)

    try:
        overlay = wait_for(lambda: next(n for n in nodes()
                                        if n.get_name().startswith('Screenshot text selection.')))
        window_id = subprocess.check_output(['xdotool', 'search', '--name', 'Ranslator — select text'], text=True).strip().splitlines()[-1]
        mouse('windowfocus', window_id)
        assert not overlay.get_description(), 'Fixture OCR should still be delayed'
        for i, y in enumerate((180, 330, 480)):
            if i == 0:
                mouse('keydown', 'Shift_L')
            mouse('mousemove', 82, y - 5, 'mousedown', 1, 'mousemove', 345, y + 45, 'mouseup', 1)
            if i == 0:
                mouse('keyup', 'Shift_L')
            wait_for(lambda: find(f'Snapshot translation {i + 1}'))
            print(f'Card {i + 1} created', flush=True)
        for i, expected in enumerate(('Первый фрагмент', 'Второй фрагмент', 'Третий фрагмент'), 1):
            wait_for(lambda: Atspi.Text.get_text(find(f'Translated text {i}'), 0, -1) == expected)
        assert completed == ['Second region', 'Third region', 'First region'], completed
        assert not any(n.get_name() == 'Quick translated text' and
                       n.get_state_set().contains(Atspi.StateType.SHOWING) for n in nodes())
        wait_for(lambda: overlay.get_description())
        # Move a card across the frozen screen with real mouse input.
        card = find('Snapshot translation 1')
        before = Atspi.Component.get_extents(card, Atspi.CoordType.SCREEN)
        print(f'Drag from card at {before.x}, {before.y}', flush=True)
        mouse('mousemove', before.x + 90, before.y + 28, 'mousedown', 1,
              'sleep', .1, 'mousemove', 760, 320, 'sleep', .1, 'mouseup', 1)
        def moved():
            after = Atspi.Component.get_extents(card, Atspi.CoordType.SCREEN)
            return (after.x, after.y) != (before.x, before.y)
        wait_for(moved)
        time.sleep(.15)
        (ROOT / 'build').mkdir(exist_ok=True)
        ImageGrab.grab(xdisplay=os.environ['DISPLAY']).save(ROOT / 'build/multi-snapshot.png')
        mouse('key', 'Escape')
        wait_for(lambda: not any(n.get_name().startswith('Screenshot text selection.') and
                                 n.get_state_set().contains(Atspi.StateType.SHOWING) for n in nodes()))
        print('PASS: three early selections → real crop OCR → bounded concurrent translations → '
              'correct cards despite out-of-order replies → drag → Escape. User desktop untouched.')
    except Exception:
        ImageGrab.grab(xdisplay=os.environ['DISPLAY']).save(ROOT / 'build/multi-snapshot-failure.png')
        for node in nodes():
            if node.get_state_set().contains(Atspi.StateType.SHOWING):
                print(node.get_name(), node.get_description(), flush=True)
        raise
    finally:
        app.terminate()
        app.wait(timeout=5)
        server.shutdown()
        display.terminate()
        display.wait(timeout=5)
