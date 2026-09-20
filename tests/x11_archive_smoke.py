#!/usr/bin/env python3
"""Real archive UI, private destinations, cancellation and worker cleanup."""
import os
import pathlib
import signal
import threading
import time
import zipfile
from x11_desktop_smoke import run, launch, close, wait_for
from x11_core_apps_smoke import keys, type_text

home = pathlib.Path(os.environ['WIIDESK_TEST_HOME'])
archive = home / 'fixture.zip'
with zipfile.ZipFile(archive, 'w') as z:
    z.writestr('docs/', '')
    z.writestr('docs/hello.txt', 'hello Wii\n')
window = launch(11, '^WiiDesk Archives$')


def status():
    return run('xprop', '-id', window, '_WIIDESK_ARCHIVE_STATUS')


def output():
    return pathlib.Path(run('xprop', '-id', window, '_WIIDESK_ARCHIVE_OUTPUT').split('"')[1])


type_text(str(archive)); keys('Return')
wait_for(lambda: 'Checked: 2 entries' in status(), 'list archive')
keys('ctrl+Return')
wait_for(lambda: 'Extracted: 2 entries' in status(), 'extract archive')
first = output()
assert first.parent == home
assert (first / 'docs/hello.txt').read_text() == 'hello Wii\n'
assert not (first / '.wiidesk-incomplete').exists()
keys('ctrl+Return')
wait_for(lambda: 'Extracted: 2 entries' in status() and output() != first, 'second extraction gets a new directory')
bad = home / 'unsafe.zip'
with zipfile.ZipFile(bad, 'w') as z:
    z.writestr('../escape.txt', 'must not escape')
keys('ctrl+a'); type_text(str(bad)); keys('ctrl+Return')
wait_for(lambda: 'Unsafe' in status() and 'INCOMPLETE' in status(), 'unsafe extraction fails visibly')
assert (output() / '.wiidesk-incomplete').exists()
assert not (home / 'escape.txt').exists()

large = home / 'slow.zip'
with zipfile.ZipFile(large, 'w', compression=zipfile.ZIP_DEFLATED) as z:
    z.writestr('large.txt', b'x' * (20 * 1024 * 1024))
pid = int(run('xprop', '-id', window, '_NET_WM_PID').split('=')[1])


def stop_decoder():
    keys('ctrl+a'); type_text(str(large))
    captured = []

    def stop_child():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for child in pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split():
                try:
                    os.kill(int(child), signal.SIGSTOP)
                except ProcessLookupError:
                    continue
                captured.append(int(child)); return
            time.sleep(.0005)

    thread = threading.Thread(target=stop_child)
    thread.start(); keys('ctrl+Return'); thread.join()
    assert captured
    wait_for(lambda: 'Extracting...' in status(), 'UI active with stopped worker')
    return pathlib.Path(f'/proc/{captured[0]}'), output()


child, partial = stop_decoder()
keys('Escape')
wait_for(lambda: 'Cancelled' in status() and not child.exists(), 'cancel reaps worker')
assert (partial / '.wiidesk-incomplete').exists()
child, partial = stop_decoder()
close(window)
wait_for(lambda: not child.exists(), 'close reaps worker')
assert (partial / '.wiidesk-incomplete').exists()
print('PASS: Archives launcher/list/extract, new destinations, unsafe paths, markers, cancellation and close cleanup')
