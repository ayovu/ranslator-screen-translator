#!/usr/bin/python3
"""Live KDE capture test. Launch Ranslator first. Temporarily shows a synthetic fullscreen.
Only translates if OCR matches the fixture. Calls the configured translation provider.
Needs a C++ compiler, Qt6Widgets pkg-config, qdbus6, Python GObject and AT-SPI.
"""
import pathlib
import subprocess
import time
import gi

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURE = 'Ranslator snapshot test. This sentence is ready to translate.'

def tree(node):
    yield node
    for child in node:
        if child.get_role_name() not in ('list', 'menu'):
            yield from tree(child)

def nodes():
    app = next(a for a in Atspi.get_desktop(0) if a.get_name() == 'ranslator')
    return list(tree(app))

def find(name):
    return next(n for n in nodes() if n.get_name() == name)

def wait_for(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (StopIteration, gi.repository.GLib.Error):
            pass
        time.sleep(.2)
    raise RuntimeError('Timed out waiting for the snapshot workflow')

(ROOT / 'build').mkdir(exist_ok=True)
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Widgets'], text=True).split()
executable = ROOT / 'build/screen-fixture'
subprocess.run(['c++', '-fPIC', str(ROOT / 'tests/fixtures/screen.cpp'), '-o', str(executable), *flags], check=True)

fixture = subprocess.Popen([str(executable)])
try:
    time.sleep(1)
    started = time.monotonic()
    subprocess.run(['qdbus6', 'org.kde.kglobalaccel', '/component/ranslator',
                    'org.kde.kglobalaccel.Component.invokeShortcut', 'snapshot'], check=True)
    overlay = wait_for(lambda: next(n for n in nodes() if n.get_name().startswith('Screenshot text selection.')))
    frozen = time.monotonic()
    wait_for(lambda: ' '.join(overlay.get_description().split()) == FIXTURE)
    recognized = time.monotonic()
    assert not any(n.get_name() == 'Text to translate' and n.get_state_set().contains(Atspi.StateType.SHOWING) for n in nodes()), 'Full application appeared during capture'
    print(f'Frozen overlay: {frozen-started:.2f}s; OCR ready: {recognized-started:.2f}s')
    control = find('Snapshot controls')
    action = next(i for i in range(Atspi.Action.get_n_actions(control)) if Atspi.Action.get_action_name(control, i).lower() == 'press')
    Atspi.Action.do_action(control, action)
    Atspi.Action.do_action(find('Zoom in'), 0)
    wait_for(lambda: find('140%'))
    Atspi.Action.do_action(find('Fit'), 0)
    wait_for(lambda: find('100%'))
    Atspi.Action.do_action(find('Select all'), 0)
    Atspi.Action.do_action(find('Translate selection'), 0)
    # An older translation can remain visible until the queued selection arrives.
    wait_for(lambda: Atspi.Text.get_text(find('Quick translated text'), 0, -1), seconds=100)
    assert not any(n.get_name() == 'Text to translate' and n.get_state_set().contains(Atspi.StateType.SHOWING) for n in nodes()), 'Full application appeared for the result'
    Atspi.Action.do_action(find('Dismiss translation'), 0)
    print('PASS: KDE shortcut → screen capture → OCR → zoom/fit → quick translation → dismiss, without opening the full app.')
finally:
    try:
        Atspi.Action.do_action(find('Cancel'), 0)
    except (StopIteration, gi.repository.GLib.Error):
        pass
    fixture.terminate()
    fixture.wait(timeout=5)
