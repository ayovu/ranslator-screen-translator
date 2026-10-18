"""Persistent test-only virtual pointer for a private, disposable Wayland session.

The small wire client uses wl_display/wl_registry and version 1 of
https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/blob/master/unstable/wlr-virtual-pointer-unstable-v1.xml
It keeps the seat's pointer capability alive before the app connects, then sends
separated input events. There are no third-party Python package requirements.
"""
import os
from pathlib import Path
import socket
import struct
import time

class Pointer:
    def __init__(self):
        assert os.environ.get('RANSLATOR_WLROOTS_LAB') == '1'
        assert Path('/run/.containerenv').exists() or Path('/.dockerenv').exists()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect(str(Path(os.environ['XDG_RUNTIME_DIR']) / os.environ['WAYLAND_DISPLAY']))
        self.buffer = b''
        self.globals = {}
        self.next_sync = 3
        self.send(1, 1, self.ints(2))
        self.sync()
        self.bind('zwlr_virtual_pointer_manager_v1', 4)
        self.bind('wl_seat', 5)
        self.send(4, 0, self.ints(5, 6))
        self.next_sync = 7
        self.sync()
    @staticmethod
    def ints(*values):
        return struct.pack('=' + 'I' * len(values), *values)
    @staticmethod
    def string(value):
        raw = value.encode() + b'\0'
        return struct.pack('=I', len(raw)) + raw + b'\0' * (-len(raw) % 4)
    def send(self, object_id, opcode, payload=b''):
        self.sock.sendall(self.ints(object_id, ((len(payload) + 8) << 16) | opcode) + payload)
    def event(self):
        while len(self.buffer) < 8:
            self.receive()
        object_id, word = struct.unpack_from('=II', self.buffer)
        length = word >> 16
        assert length >= 8
        while len(self.buffer) < length:
            self.receive()
        payload, self.buffer = self.buffer[8:length], self.buffer[length:]
        return object_id, word & 65535, payload
    def receive(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError('Disposable Wayland compositor disconnected')
        self.buffer += data
    def sync(self):
        callback = self.next_sync
        self.next_sync += 1
        self.send(1, 0, self.ints(callback))
        while True:
            object_id, opcode, payload = self.event()
            if object_id == 1 and opcode == 0:
                raise RuntimeError(f'Wayland protocol error: {payload!r}')
            if object_id == 2 and opcode == 0:
                name, size = struct.unpack_from('=II', payload)
                interface = payload[8:8 + size - 1].decode()
                self.globals[interface] = name
            if object_id == callback:
                return
    def bind(self, interface, object_id):
        self.send(2, 0, self.ints(self.globals[interface]) + self.string(interface) + self.ints(1, object_id))
    def move(self, x, y):
        self.send(6, 1, self.ints(int(time.monotonic() * 1000) & 0xffffffff, x, y, 2400, 800))
        self.send(6, 4)
        self.sync()
    def button(self, pressed):
        self.send(6, 2, self.ints(int(time.monotonic() * 1000) & 0xffffffff, 272, int(pressed)))
        self.send(6, 4)
        self.sync()
    def close(self):
        self.send(6, 8)
        self.send(4, 1)
        self.sock.close()
