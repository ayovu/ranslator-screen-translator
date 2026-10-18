#!/usr/bin/python3
"""Live KDE monitor regression. Briefly covers all monitors with synthetic fixtures.
Run with the regular app stopped. Uses a temporary profile; no translation API is called.
Requires Qt6Widgets, xdotool, XWayland, qdbus6 and Python GObject/AT-SPI.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import gi

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib

ROOT = Path(__file__).resolve().parent.parent


def run(*args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, timeout=8, **kwargs).strip()


def nodes(node=None):
    if node is None:
        node = next((a for a in Atspi.get_desktop(0) if a.get_name() == 'ranslator'), None)
    if node is None:
        return
    yield node
    for child in node:
        if child.get_role_name() not in ('list', 'menu'):
            yield from nodes(child)


def wait_for(predicate, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            value = predicate()
            if value:
                return value
        except (StopIteration, GLib.Error):
            pass
        time.sleep(.08)
    raise AssertionError('Monitor regression timed out')


def overlay():
    return next(n for n in nodes() if n.get_name().startswith('Screenshot text selection.') and
                n.get_state_set().contains(Atspi.StateType.SHOWING))


def cancel():
    button = next(n for n in nodes() if n.get_name() == 'Cancel' and
                  n.get_state_set().contains(Atspi.StateType.SHOWING))
    Atspi.Action.do_action(button, 0)
    # Allow the compositor's close animation to finish before the next screenshot.
    time.sleep(.5)


flags = run('pkg-config', '--cflags', '--libs', 'Qt6Widgets').split()
executable = ROOT / 'build/screen-fixture'
subprocess.run(['c++', '-fPIC', str(ROOT / 'tests/fixtures/screen.cpp'), '-o', str(executable), *flags], check=True)
if any(a.get_name() == 'ranslator' for a in Atspi.get_desktop(0)):
    raise SystemExit('Stop Ranslator before this test; it uses an isolated temporary profile.')

old_pointer = dict(line.split('=') for line in run('xdotool', 'getmouselocation', '--shell').splitlines())
fixture = subprocess.Popen([str(executable), '--all-monitors'], env=os.environ | {'QT_QPA_PLATFORM': 'xcb'},
                           stdout=subprocess.PIPE, text=True)
monitors = json.loads(fixture.stdout.readline())
app = None
try:
    assert len(monitors) >= 2, 'This regression needs two monitors'
    time.sleep(.5)
    with tempfile.TemporaryDirectory(prefix='ranslator-monitor-check-') as temporary:
        tmp = Path(temporary)
        config = json.loads((ROOT / 'config.example.json').read_text())
        config.update(provider='compatible', endpoint='http://127.0.0.1:9/v1', model='unused',
                      ocr_languages='eng')
        env = os.environ | {'RANSLATOR_CONFIG': str(tmp / 'config.json'), 'TMPDIR': temporary,
                            'QT_QPA_PLATFORM': 'wayland'}
        for mode in ('auto', 'desktop'):
            config['capture_mode'] = mode
            (tmp / 'config.json').write_text(json.dumps(config))
            app = subprocess.Popen([str(ROOT / 'target/release/ranslator'), '--background'], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wait_for(lambda: next(iter(nodes()), None))
            time.sleep(.4)
            for monitor in monitors:
                run('xdotool', 'windowactivate', '--sync', monitor['window'])
                run('xdotool', 'mousemove', monitor['x'] + monitor['width'] // 2,
                    monitor['y'] + monitor['height'] // 2)
                wait_for(lambda: run('qdbus6', 'org.kde.KWin', '/KWin',
                                     'org.kde.KWin.activeOutputName') == monitor['name'])
                time.sleep(.3)
                run('qdbus6', 'org.kde.kglobalaccel', '/component/ranslator',
                    'org.kde.kglobalaccel.Component.invokeShortcut', 'snapshot')
                view = wait_for(overlay)
                expected = [monitor['text']] if mode == 'auto' else [m['text'] for m in monitors]
                try:
                    recognized = wait_for(lambda: view.get_description() if
                                          all(text in view.get_description() for text in expected) else None, seconds=12)
                except AssertionError:
                    print('OCR description:', repr(view.get_description()), flush=True)
                    print('Showing controls:', [n.get_name() for n in nodes() if n.get_state_set().contains(Atspi.StateType.SHOWING)], flush=True)
                    raise
                if mode == 'auto':
                    assert recognized.strip() == monitor['text'], recognized
                extent = Atspi.Component.get_extents(view, Atspi.CoordType.SCREEN)
                assert (extent.width, extent.height) == (monitor['width'], monitor['height']), (
                    mode, monitor['name'], extent.width, extent.height)
                print(f'PASS {mode}: capture and overlay on {monitor["name"]}', flush=True)
                cancel()
            subprocess.run([str(ROOT / 'target/release/ranslator'), '--quit'], env=env, check=True)
            app.wait(timeout=5)
            app = None
            time.sleep(.2)
finally:
    if app is not None:
        app.terminate()
        app.wait(timeout=5)
    fixture.terminate()
    fixture.wait(timeout=5)
    run('xdotool', 'mousemove', old_pointer['X'], old_pointer['Y'])
