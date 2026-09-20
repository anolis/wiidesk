#!/usr/bin/env python3
"""Exercise extraction against real hostile archive fixtures in private folders."""
import io
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import zipfile

probe = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='wiidesk-archive-test.') as tmp:
    root = pathlib.Path(tmp)
    archive = root / 'sample.zip'
    out = root / 'out'
    out.mkdir()

    def run(path, destination=None, ok=True):
        p = subprocess.run([probe, str(path)] + ([str(destination)] if destination else []),
                           capture_output=True, text=True, timeout=15)
        assert (p.returncode == 0) == ok, (p.returncode, p.stdout, p.stderr)
        return p.stdout

    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('folder/', b'')
        z.writestr('folder/hello.txt', b'hello Wii\n')
    assert 'OK entries=2 bytes=10' in run(archive)
    run(archive, out)
    assert (out / 'folder/hello.txt').read_bytes() == b'hello Wii\n'
    assert (out / 'folder/hello.txt').stat().st_mode & 0o777 == 0o600
    run(archive, out, ok=False)  # Must never overwrite.
    for name in ('../escaped', '/tmp/escaped', 'a/../../escaped', 'C:/escaped',
                 '..\\escaped', 'a\nbad', '.wiidesk-incomplete', '/absolute'):
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr(name, 'danger')
        run(archive, out, ok=False)
    assert not (root / 'escaped').exists()
    for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE):
        with tarfile.open(root / 'bad.tar', 'w') as t:
            entry = tarfile.TarInfo('link'); entry.type = kind; entry.linkname = '../escaped'
            t.addfile(entry)
        run(root / 'bad.tar', out, ok=False)
    # Existing symlink directory must not be followed, even with a safe pathname.
    outside = root / 'outside'; outside.mkdir()
    (out / 'pivot').symlink_to(outside, target_is_directory=True)
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('pivot/escaped', b'no')
    run(archive, out, ok=False)
    assert not (outside / 'escaped').exists()
    # Non-seekable ZIP writers use data descriptors rather than header sizes.
    class Streaming(io.BytesIO):
        def seekable(self): return False
        def seek(self, *args): raise io.UnsupportedOperation('stream')
    stream = Streaming()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('streamed.txt', b'streamed bytes')
    archive.write_bytes(stream.getvalue())
    target = root / 'stream'; target.mkdir()
    run(archive, target)
    assert (target / 'streamed.txt').read_bytes() == b'streamed bytes'
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('same', 'one'); z.writestr('./same', 'two')
    run(archive, ok=False)
    with zipfile.ZipFile(archive, 'w') as z:
        for i in range(513): z.writestr(f'f{i}', '')
    assert '512 entries' in run(archive, ok=False)
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('/'.join(['deep'] * 17), '')
    run(archive, ok=False)
    # Declared sizes are rejected before trying to allocate/decode their data.
    with tarfile.open(root / 'large.tar', 'w') as t:
        entry = tarfile.TarInfo('huge'); entry.size = 32 * 1024 * 1024 + 1
        t.addfile(entry)
    assert 'Size limit' in run(root / 'large.tar', ok=False)
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('boundary', b'x' * (32 * 1024 * 1024))
    assert 'bytes=33554432' in run(archive)
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for i in range(3): z.writestr(f'large-{i}', b'x' * (24 * 1024 * 1024))
    assert 'Size limit' in run(archive, ok=False)
    oversized = root / 'encoded-too-large'
    with oversized.open('wb') as f: f.truncate(32 * 1024 * 1024 + 1)
    assert 'at most 32 MiB' in run(oversized, ok=False)
    for mode, suffix in (('w:gz', 'gz'), ('w:bz2', 'bz2'), ('w:xz', 'xz')):
        p = root / f'good.tar.{suffix}'
        with tarfile.open(p, mode) as t:
            entry = tarfile.TarInfo('./nested/file'); entry.size = 3
            t.addfile(entry, io.BytesIO(b'abc'))
        target = root / suffix; target.mkdir()
        run(p, target)
        assert (target / 'nested/file').read_bytes() == b'abc'
    # Payload corruption must fail during listing as well as extraction.
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_STORED) as z:
        z.writestr('crc', b'UNIQUE-DATA')
    data = archive.read_bytes().replace(b'UNIQUE-DATA', b'BROKEN-DATA')
    archive.write_bytes(data)
    run(archive, ok=False)
    (root / 'truncated').write_bytes(data[:40]); run(root / 'truncated', ok=False)
    os.mkfifo(root / 'pipe'); run(root / 'pipe', ok=False)
    run(root, ok=False)
    (root / 'empty').touch(); run(root / 'empty', ok=False)
print('PASS: ZIP/tar codecs, contents/modes, unsafe paths, links/devices, no-overwrite, symlink pivot, duplicate/size/count limits, CRC and FIFO rejection')
