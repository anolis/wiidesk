#!/usr/bin/env python3
"""Bounded real-codec fixtures, including malformed and special-file inputs."""
import ctypes
import pathlib
import sys
import tempfile
from PIL import Image


class Result(ctypes.Structure):
    _fields_ = [('width', ctypes.c_uint), ('height', ctypes.c_uint),
                ('ok', ctypes.c_int), ('path', ctypes.c_char * 4096),
                ('error', ctypes.c_char * 192),
                ('rgb', ctypes.c_ubyte * (1024 * 1024 * 3))]


lib = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
lib.image_decode.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(Result)]
with tempfile.TemporaryDirectory(prefix='wiidesk-image-test.') as tmp:
    folder = pathlib.Path(tmp)
    red = folder / 'a.png'
    green = folder / 'b.jpg'
    Image.new('RGBA', (13, 7), (255, 0, 0, 255)).save(red)
    Image.new('RGB', (19, 11), (0, 255, 0)).save(green, quality=95, progressive=True)

    def decode(path, direction=0):
        result = Result()
        lib.image_decode(str(path).encode(), direction, ctypes.byref(result))
        return result

    r = decode(red)
    assert r.ok and (r.width, r.height) == (13, 7) and bytes(r.rgb[:3]) == b'\xff\0\0'
    r = decode(red, 1)
    assert r.ok and (r.width, r.height) == (19, 11) and r.rgb[1] >= 253
    assert decode(green, -1).ok
    assert not decode(red, -1).ok
    alpha = folder / 'alpha.png'
    Image.new('RGBA', (2, 2), (255, 0, 0, 0)).save(alpha)
    assert bytes(decode(alpha).rgb[:3]) == bytes((36, 43, 47))
    gray = folder / 'gray.png'
    Image.new('L', (2, 2), 127).save(gray)
    assert bytes(decode(gray).rgb[:3]) == bytes((127, 127, 127))
    boundary = folder / 'boundary.png'
    Image.new('RGB', (1024, 1024), (10, 20, 30)).save(boundary)
    r = decode(boundary)
    assert r.ok and (r.width, r.height) == (1024, 1024)
    assert bytes(r.rgb[-3:]) == bytes((10, 20, 30))
    large = folder / 'large.png'
    Image.new('RGB', (2048, 1024), 'blue').save(large)
    r = decode(large)
    assert not r.ok and b'1 megapixel' in r.error
    broken = folder / 'broken.jpg'
    broken.write_bytes(green.read_bytes()[:70])
    assert not decode(broken).ok
    broken.write_bytes(b'not an image')
    assert not decode(broken).ok
    import os
    fifo = folder / 'pipe.png'
    os.mkfifo(fifo)
    assert not decode(fifo).ok
    assert not decode(folder).ok
    assert not decode(folder / 'missing').ok
    huge = folder / 'huge.png'
    with huge.open('wb') as f:
        f.truncate(16 * 1024 * 1024 + 1)
    assert not decode(huge).ok
    # Bound directory browsing independently of image decoding.
    for i in range(257):
        (folder / f'picture-{i}.png').touch()
    r = decode(red, 1)
    assert not r.ok and b'Browse limit' in r.error
print('PASS: PNG/JPEG pixels, alpha/gray, browse, dimension/file bounds, malformed images and FIFO rejection')
