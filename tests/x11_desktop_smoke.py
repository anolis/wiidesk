#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the launcher and companion apps inside the isolated test runner."""
import ctypes
import ctypes.util
import os
import pathlib
import subprocess
import time


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def wait_for(check, message):
    for _ in range(100):
        if check():
            return
        time.sleep(.05)
    raise AssertionError(message)


def windows(title):
    result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', title],
                            capture_output=True, text=True)
    return result.stdout.strip().splitlines() if result.returncode == 0 else []


def launch(index, title):
    run('xdotool', 'key', '--clearmodifiers', 'alt+F1')
    wait_for(lambda: windows('^WiiDesk Launcher$'), 'launcher opens')
    for _ in range(index):
        run('xdotool', 'key', 'Down')
    run('xdotool', 'key', 'Return')
    wait_for(lambda: windows(title), f'{title} opens')
    return windows(title)[0]


def close(window):
    run('xdotool', 'windowactivate', '--sync', window, 'key', 'alt+F4')
    wait_for(lambda: subprocess.run(['xdotool', 'getwindowname', window],
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL).returncode != 0,
             'application exits on WM_DELETE_WINDOW')


def desktop_pixel():
    # Read an uncovered root pixel to verify that Save actually reaches the WM.
    xlib = ctypes.CDLL(ctypes.util.find_library('X11'))
    xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
    xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    xlib.XDefaultRootWindow.restype = ctypes.c_ulong
    xlib.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                              ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                              ctypes.c_ulong, ctypes.c_int]
    xlib.XGetImage.restype = ctypes.c_void_p
    xlib.XGetPixel.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    xlib.XGetPixel.restype = ctypes.c_ulong
    xlib.XDestroyImage.argtypes = [ctypes.c_void_p]
    xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = xlib.XOpenDisplay(None)
    assert display
    image = xlib.XGetImage(display, xlib.XDefaultRootWindow(display),
                          620, 20, 1, 1, ctypes.c_ulong(-1), 2)
    assert image
    pixel = xlib.XGetPixel(image, 0, 0)
    xlib.XDestroyImage(image)
    xlib.XCloseDisplay(display)
    return pixel


def main():
    home = pathlib.Path(os.environ['WIIDESK_TEST_HOME'])
    assert str(home) == os.environ['HOME'] and home.name == 'home'
    (home / 'folder').mkdir()
    (home / 'sample.txt').write_text('WiiDesk file metadata fixture\n')
    files = launch(1, '^WiiDesk Files')
    run('xdotool', 'key', 'Return')
    wait_for(lambda: run('xdotool', 'getwindowname', files).endswith('/folder'),
             'Enter opens selected folder')
    run('xdotool', 'key', 'BackSpace')
    wait_for(lambda: run('xdotool', 'getwindowname', files).endswith(str(home)),
             'Backspace returns to parent')
    run('xdotool', 'key', 'Down', 'Return', 'F5')
    assert run('xdotool', 'getwindowname', files).endswith(str(home))
    close(files)
    system = launch(2, '^WiiDesk System$')
    time.sleep(1.2)
    run('xdotool', 'key', 'alt+F9')
    wait_for(lambda: 'HIDDEN' in run('xprop', '-id', system, '_NET_WM_STATE'), 'system minimizes')
    run('xdotool', 'windowactivate', '--sync', system)
    close(system)
    original_pixel = desktop_pixel()
    settings = launch(3, '^WiiDesk Settings$')
    run('xdotool', 'key', 'Right', 'Down', 'Right', 'Return')
    config = home / '.config' / 'wiidesk' / 'x11.conf'
    wait_for(config.exists, 'settings are saved')
    assert config.read_text() == 'background=1\naccent=1\n', config.read_text()
    wait_for(lambda: desktop_pixel() != original_pixel, 'saved background applied by WM')
    close(settings)
    # Restore defaults and check that reopening loads the saved values.
    settings = launch(3, '^WiiDesk Settings$')
    run('xdotool', 'key', 'Left', 'Down', 'Left', 'Return')
    wait_for(lambda: config.read_text() == 'background=0\naccent=0\n', 'settings reload and save')
    wait_for(lambda: desktop_pixel() == original_pixel, 'default background restored')
    close(settings)
    run('xdotool', 'key', 'alt+F1')
    wait_for(lambda: windows('^WiiDesk Launcher$'), 'menu reopens')
    run('xdotool', 'key', 'Escape')
    wait_for(lambda: not windows('^WiiDesk Launcher$'), 'Escape dismisses menu')
    # Click the panel and select Files with the pointer.
    run('xdotool', 'mousemove', '30', '466', 'click', '1')
    wait_for(lambda: windows('^WiiDesk Launcher$'), 'panel opens launcher')
    run('xdotool', 'mousemove', '35', '371', 'click', '1')
    wait_for(lambda: windows('^WiiDesk Files'), 'mouse launches Files')
    close(windows('^WiiDesk Files')[0])
    print('PASS: launcher keyboard/mouse, Files navigation, System hide/restore, Settings persistence, polite close')


if __name__ == '__main__':
    main()
