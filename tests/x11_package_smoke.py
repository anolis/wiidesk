#!/usr/bin/env python3
"""Inspection UI coverage; privileged installation is tested on target only."""
import os
import pathlib
import signal
import threading
import time
from package_io_test import fixture
from x11_desktop_smoke import run, launch, close, wait_for, windows
from x11_core_apps_smoke import keys, type_text

home = pathlib.Path(os.environ['WIIDESK_TEST_HOME'])
package = fixture(home)
window = launch(12, '^WiiDesk Packages$')


def status():
    return run('xprop', '-id', window, '_WIIDESK_PACKAGE_STATUS')


def job():
    return pathlib.Path(run('xprop', '-id', window, '_WIIDESK_PACKAGE_JOB').split('"')[1])


type_text(str(package)); keys('Return')
wait_for(lambda: 'Review 1 package' in status(), 'review metadata')
first = job()
assert (first / '00.deb').read_bytes() == package.read_bytes()
keys('F2')
wait_for(lambda: windows('^WiiDesk Log'), 'open package details')
close(windows('^WiiDesk Log')[0])
run('xdotool', 'windowactivate', '--sync', window)
keys('End'); type_text('x')
wait_for(lambda: 'Path edited' in status(), 'edited path invalidates review')
keys('ctrl+Return'); time.sleep(.2)
assert not windows('^WiiDesk Package Installation$')
keys('BackSpace', 'Return')
wait_for(lambda: 'Review 1 package' in status(), 'inspect again')
assert not first.exists()
pid = int(run('xprop', '-id', window, '_NET_WM_PID').split('=')[1])


def stop_worker():
    captured = []

    def watch():
        until = time.monotonic() + 5
        while time.monotonic() < until:
            for child in pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split():
                try:
                    os.kill(int(child), signal.SIGSTOP)
                except ProcessLookupError:
                    continue
                captured.append(int(child)); return
            time.sleep(.0005)

    thread = threading.Thread(target=watch)
    thread.start(); keys('Return'); thread.join()
    assert captured
    wait_for(lambda: 'Inspecting snapshots' in status(), 'stopped inspection')
    return pathlib.Path(f'/proc/{captured[0]}'), job()


child, staging = stop_worker()
keys('Escape')
wait_for(lambda: 'cancelled' in status() and not child.exists(), 'cancel reaps stopped worker')
assert not list(staging.glob('*.deb'))
child, staging = stop_worker()
close(window)
wait_for(lambda: not child.exists() and not staging.exists(), 'close cleans inspector and snapshots')
print('PASS: Packages launcher, inspection, details, review invalidation, cancellation and close cleanup')
