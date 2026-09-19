#!/usr/bin/env python3
"""Image viewer controls and real XImage pixels (including 16-bit visuals)."""
import ctypes
import ctypes.util
import os
import pathlib
import signal
import threading
import time
from PIL import Image
from x11_desktop_smoke import run, launch, close, wait_for
from x11_core_apps_smoke import keys, type_text


def pixel(window):
    lib = ctypes.CDLL(ctypes.util.find_library('X11'))
    lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
    lib.XOpenDisplay.restype = ctypes.c_void_p
    lib.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                             ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
    lib.XGetImage.restype = ctypes.c_void_p
    lib.XGetPixel.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.XGetPixel.restype = ctypes.c_ulong
    lib.XDestroyImage.argtypes = [ctypes.c_void_p]
    lib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = lib.XOpenDisplay(None)
    image = lib.XGetImage(display, int(window), 270, 200, 1, 1, ctypes.c_ulong(-1), 2)
    assert image
    result = lib.XGetPixel(image, 0, 0)
    lib.XDestroyImage(image)
    lib.XCloseDisplay(display)
    return result


folder = pathlib.Path(os.environ['WIIDESK_TEST_HOME']) / 'pictures'
folder.mkdir()
Image.new('RGB', (100, 60), 'red').save(folder / 'a.png')
Image.new('RGB', (80, 50), (0, 255, 0)).save(folder / 'b.jpg', quality=100)
window = launch(10, '^WiiDesk Images$')


def status():
    return run('xprop', '-id', window, '_WIIDESK_IMAGE_STATUS')


type_text(str(folder / 'a.png'))
keys('Return')
wait_for(lambda: '100 x 60 | Fit' in status(), 'PNG opens')
assert pixel(window) in (0xff0000, 0xf800), hex(pixel(window))
run('xdotool', 'mousemove', '--window', window, '295', '55', 'click', '1')
wait_for(lambda: 'Zoom 100%' in status(), 'actual-size button')
keys('F5', 'Page_Down')
wait_for(lambda: '80 x 50 | Fit' in status(), 'next JPEG opens')
assert pixel(window) in (0x00ff01, 0x00ff00, 0x07e0), hex(pixel(window))
keys('Page_Up')
wait_for(lambda: '100 x 60 | Fit' in status(), 'previous PNG opens')
keys('ctrl+a')
type_text(str(folder / 'missing.png'))
keys('Return')
wait_for(lambda: 'Open:' in status(), 'missing file error displayed')
# A FIFO must fail promptly and leave controls responsive.
os.mkfifo(folder / 'pipe.png')
keys('ctrl+a')
type_text(str(folder / 'pipe.png'))
keys('Return')
wait_for(lambda: 'regular image file' in status(), 'FIFO does not block the UI')

# Stop only this fixture's decoder to model stalled I/O, then check that
# Cancel and window close both reap it without freezing the event loop.
Image.effect_noise((1024, 1024), 64).convert('RGB').save(folder / 'slow.png')
pid = int(run('xprop', '-id', window, '_NET_WM_PID').split('=')[1])


def stopped_decoder():
    keys('ctrl+a')
    type_text(str(folder / 'slow.png'))
    children = pathlib.Path(f'/proc/{pid}/task/{pid}/children')
    captured = []

    def stop_child():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for child in children.read_text().split():
                try:
                    os.kill(int(child), signal.SIGSTOP)
                except ProcessLookupError:
                    continue
                captured.append(int(child))
                return
            time.sleep(.001)

    thread = threading.Thread(target=stop_child)
    thread.start()
    keys('Return')
    thread.join()
    assert captured, 'decoder available for cancellation test'
    wait_for(lambda: 'Loading image' in status(), 'UI remains active while decoder stopped')
    return pathlib.Path(f'/proc/{captured[0]}')


decoder = stopped_decoder()
keys('Escape')
wait_for(lambda: 'cancelled' in status() and not decoder.exists(), 'Cancel reaps stalled worker')
decoder = stopped_decoder()
close(window)
wait_for(lambda: not decoder.exists(), 'close reaps stalled worker')
print('PASS: Image Viewer launcher, PNG/JPEG pixels, fit/actual-size, browsing, errors, cancellation and close cleanup')
