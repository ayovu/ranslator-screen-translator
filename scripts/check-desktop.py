#!/usr/bin/python3
"""Live KDE smoke test. Run the app first. Temporarily replaces PRIMARY, then restores it.
Uses accessibility actions on Ranslator only; never reads other application windows.
"""
import subprocess
import time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

def tree(node):
    yield node
    for child in node:
        if child.get_role_name() not in ('list', 'menu'):
            yield from tree(child)

def app():
    return next(a for a in Atspi.get_desktop(0) if a.get_name() == 'ranslator')

def find(name):
    return next(x for x in tree(app()) if x.get_name() == name)

def press(name):
    node = find(name)
    # A checkable button exposes Toggle before Press; Toggle changes its state
    # without activating the button's clicked action.
    index = next((i for i in range(Atspi.Action.get_n_actions(node))
                  if Atspi.Action.get_action_name(node, i).lower() == 'press'), None)
    if index is None:
        return False
    return Atspi.Action.do_action(node, index)

original = subprocess.run(['wl-paste', '--primary', '--no-newline', '--type', 'text'], capture_output=True)
try:
    subprocess.run(['qdbus6', 'org.kde.kglobalaccel', '/component/ranslator', 'org.kde.kglobalaccel.Component.invokeShortcut', 'once'], check=True)
    time.sleep(0.4)
    subprocess.run(['wl-copy', '--primary', '--type', 'text/plain;charset=utf-8'], input=b'This is a small native translator for Linux.', check=True)
    deadline = time.monotonic() + 100
    while time.monotonic() < deadline:
        try:
            popup = find('Quick translation')
            output = Atspi.Text.get_text(find('Quick translated text'), 0, -1)
            source = popup.get_description()
            showing = popup.get_state_set().contains(Atspi.StateType.SHOWING)
        except StopIteration:
            output, source, showing = '', '', False
        if showing and output and source == 'This is a small native translator for Linux.':
            print('PASS: PRIMARY selection reached the native widget and produced a translation.')
            break
        time.sleep(0.5)
    else:
        raise RuntimeError('No translation arrived before the deadline')
finally:
    try:
        try:
            press('Dismiss translation')
        except StopIteration:
            pass
    finally:
        if original.returncode == 0:
            subprocess.run(['wl-copy', '--primary', '--type', 'text/plain;charset=utf-8'], input=original.stdout, check=True)
        else:
            subprocess.run(['wl-copy', '--primary', '--clear'], check=True)
