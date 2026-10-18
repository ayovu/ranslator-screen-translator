#!/usr/bin/env python3
"""Synthetic editor used only on the accessibility test's private bus/display."""
import sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

GLib.set_prgname("ranslator-gtk-fixture")
GLib.set_application_name("Selection GTK fixture")
window = Gtk.Window(title="Accessibility fixture")
editor = Gtk.TextView()
buffer = editor.get_buffer()
buffer.set_text("GTK selected phrase")
window.add(editor)
window.show_all()

def select(_source, _condition):
    if not sys.stdin.readline():
        Gtk.main_quit()
        return False
    buffer.select_range(buffer.get_start_iter(), buffer.get_end_iter())
    return True

GLib.io_add_watch(sys.stdin, GLib.IO_IN, select)
GLib.timeout_add(100, lambda: print("READY", flush=True))
Gtk.main()
