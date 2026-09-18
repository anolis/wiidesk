#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise Wi-Fi transactions against a private fake control socket, never wlan0."""
import ctypes
import pathlib
import socket
import sys
import tempfile
import threading

lib = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()), use_errno=True)
lib.wifi_connect.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                            ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_size_t]


def scenario(mode):
    with tempfile.TemporaryDirectory(prefix='wiidesk-wifi-test.') as tmp:
        path = str(pathlib.Path(tmp) / 'control')
        server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        server.bind(path)
        server.settimeout(.1)
        stopped = threading.Event()
        commands = []
        cancelled = ctypes.c_int(1 if mode == 'cancel-before' else 0)
        active = 0

        def serve():
            nonlocal active
            while not stopped.is_set():
                try:
                    data, client = server.recvfrom(4096)
                except socket.timeout:
                    continue
                command = data.decode()
                commands.append(command)
                reply = 'OK\n'
                if command == 'STATUS':
                    reply = f'id={active}\nwpa_state=COMPLETED\n'
                    if mode == 'oversize':
                        reply = 'x' * 5000
                elif command == 'ADD_NETWORK':
                    reply = '7\n'
                elif command.startswith('SELECT_NETWORK '):
                    active = int(command.split()[1])
                    if mode == 'cancel-after' and active == 7:
                        cancelled.value = 1
                elif command == 'SAVE_CONFIG' and mode == 'save-failure':
                    reply = 'FAIL\n'
                try:
                    server.sendto(reply.encode(), client)
                except FileNotFoundError:
                    pass

        worker = threading.Thread(target=serve)
        worker.start()
        message = ctypes.create_string_buffer(256)
        password = b'ab"cd\\efg'
        try:
            rc = lib.wifi_connect(path.encode(), b'Wii test', password,
                                  ctypes.byref(cancelled), message, len(message))
        finally:
            stopped.set()
            worker.join(timeout=3)
            server.close()
        assert password not in message.value
        if mode == 'success':
            assert rc == 0 and active == 7
            assert 'SET_NETWORK 7 ssid 5769692074657374' in commands
            assert 'SET_NETWORK 7 psk "ab\\"cd\\\\efg"' in commands
            assert commands[-1] == 'SAVE_CONFIG'
        else:
            assert rc != 0, mode
            if mode in ('save-failure', 'cancel-after'):
                assert active == 0
                assert commands[-2:] == ['SELECT_NETWORK 0', 'REMOVE_NETWORK 7']
            else:
                assert 'ADD_NETWORK' not in commands


for case in ('success', 'save-failure', 'cancel-before', 'cancel-after', 'oversize'):
    scenario(case)
message = ctypes.create_string_buffer(256)
cancelled = ctypes.c_int()
assert lib.wifi_connect(b'/does/not/exist', b'x', b'bad', ctypes.byref(cancelled), message, len(message)) != 0
assert b'8-63' in message.value
print('PASS: Wi-Fi profile encoding, success, save failure rollback, cancellation, oversized replies and validation')
