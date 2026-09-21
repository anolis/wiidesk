#!/usr/bin/env python3
"""Package inspection only: never installs packages or executes their scripts."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def fixture(base, name='wiidesk-package-smoke', arch='all', size=0, script=False, installed_size='1'):
    tree = base / (name + '-source')
    (tree / 'DEBIAN').mkdir(parents=True)
    (tree / 'DEBIAN/control').write_text(
        f'Package: {name}\nVersion: 0.0.1\nArchitecture: {arch}\n'
        'Maintainer: WiiDesk Tests <tests@example.invalid>\n'
        f'Installed-Size: {installed_size}\nDescription: WiiDesk harmless package test\n')
    payload = tree / 'usr/share/wiidesk/package-smoke.txt'
    payload.parent.mkdir(parents=True)
    payload.write_text('WiiDesk package installation test\n')
    if size:
        with (tree / 'usr/share/wiidesk/padding').open('wb') as f:
            f.truncate(size)
    if script:
        hook = tree / 'DEBIAN/preinst'
        hook.write_text(f'#!/bin/sh\ntouch "{base}/SCRIPT-RAN"\n')
        hook.chmod(0o755)
    output = base / (name + '.deb')
    subprocess.run(['dpkg-deb', '--build', '--root-owner-group', '-Znone', str(tree), str(output)],
                   check=True, stdout=subprocess.DEVNULL)
    return output


def main():
    probe = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='wiidesk-package-test-') as temp:
        base = pathlib.Path(temp)

        def inspect(source, expected=True, message=''):
            with tempfile.TemporaryDirectory(dir=base) as stage:
                p = subprocess.run([probe, str(source), stage], capture_output=True, text=True, timeout=10)
                assert (p.returncode == 0) == expected, p.stdout + p.stderr
                assert message in p.stdout, p.stdout
                if expected:
                    snapshot = pathlib.Path(stage) / '00.deb'
                    assert snapshot.stat().st_mode & 0o777 == 0o600
                    assert hashlib.sha256(snapshot.read_bytes()).hexdigest() in p.stdout
                return p.stdout

        good = fixture(base, script=True)
        inspect(good, message='wiidesk-package-smoke 0.0.1 all')
        assert not (base / 'SCRIPT-RAN').exists()
        overflow = fixture(base, name='wiidesk-test-overflow', installed_size='184467440737095516160')
        inspect(overflow, False, 'Invalid or overlong package metadata')
        bad = base / 'broken.deb'; bad.write_text('not a package')
        inspect(bad, False, 'Package inspection failed')
        fifo = base / 'pipe'; os.mkfifo(fifo)
        inspect(fifo, False, 'Copy failed')
        large = base / 'large.deb'
        with large.open('wb') as f: f.truncate(32*1024*1024+1)
        inspect(large, False, 'Copy failed')
        bundle = base / 'bundle'; bundle.mkdir()
        shutil.copyfile(good, bundle / 'a.deb')
        shutil.copyfile(good, bundle / 'b.deb')
        inspect(bundle, False, 'Duplicate package')
        (bundle / 'b.deb').unlink(); (bundle / 'b.deb').symlink_to(good)
        inspect(bundle, False, 'Copy failed')
        for i in range(15): (bundle / f'{i}.deb').touch()
        inspect(bundle, False, 'Bundle limit: 16')
        empty = base / 'empty'; empty.mkdir()
        inspect(empty, False, 'No .deb')
        # Three individually acceptable packages exceed the aggregate snapshot cap.
        total = base / 'total'; total.mkdir()
        for i in range(3):
            package = fixture(base, name=f'wiidesk-test-{i}', size=23*1024*1024)
            shutil.move(package, total / package.name)
        inspect(total, False, 'Copy failed')
    print('PASS: metadata/hash/overflow, no script execution, malformed input, FIFO, symlink, duplicate, file/bundle/count limits')


if __name__ == '__main__':
    main()
