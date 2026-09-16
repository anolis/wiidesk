#!/usr/bin/env python3
"""Session coordination tests. Fake lockers test supervision, not security."""
import os
import pathlib
import subprocess
import time
from x11_desktop_smoke import run, wait_for, windows, launch, close

build = pathlib.Path(os.environ['WIIDESK_TEST_BUILD'])


def status():
    return run('xprop', '-root', '_WIIDESK_SESSION_STATUS')


def click(window, y, x=50):
    run('xdotool', 'windowactivate', '--sync', window,
        'mousemove', '--window', window, str(x), str(y), 'click', '1')


def start(managed, locker):
    wm = subprocess.Popen([str(build / 'wiidesk-x11')], env={
        **os.environ, 'WIIDESK_MANAGED_SESSION': str(managed),
        'WIIDESK_LOCKER': locker})
    wait_for(lambda: 'Signed in' in status() if managed else 'Manual X11' in status(), 'WM ready')
    return wm


def stop(wm):
    if wm.poll() is None:
        wm.terminate()
    wm.wait(timeout=5)


def main():
    wm = start(0, '/bin/true')
    try:
        session = launch(6, '^WiiDesk Session$')
        click(session, 90)
        time.sleep(.1)
        assert wm.poll() is None
        assert '_WIIDESK_SESSION_CAPS(CARDINAL) = 0, 1, 0, 0' in run('xprop', '-root', '_WIIDESK_SESSION_CAPS')
        click(session, 56)
        wait_for(lambda: 'Unlocked' in status(), 'successful locker reaped')
        close(session)
    finally:
        stop(wm)

    wm = start(1, '/bin/true')
    children = []
    try:
        editor_proc = subprocess.Popen([str(build / 'wiidesk-x11-app'), 'editor'])
        children.append(editor_proc)
        wait_for(lambda: windows('^WiiDesk Editor$'), 'editor opens')
        editor = windows('^WiiDesk Editor$')[0]
        run('xdotool', 'windowactivate', '--sync', editor, 'type', 'unsaved session fixture')
        session_proc = subprocess.Popen([str(build / 'wiidesk-x11-app'), 'session'])
        children.append(session_proc)
        wait_for(lambda: windows('^WiiDesk Session$'), 'session opens')
        session = windows('^WiiDesk Session$')[0]
        click(session, 90)
        run('xdotool', 'key', 'Return')
        wait_for(lambda: 'Close or save' in status(), 'polite logout started')
        time.sleep(.2)
        assert wm.poll() is None and editor_proc.poll() is None
        click(session, 124)
        wait_for(lambda: 'Logout cancelled' in status(), 'logout cancelled')
        # Dismiss the editor's pending close prompt before another logout.
        run('xdotool', 'windowactivate', '--sync', editor, 'key', 'Escape')
        click(session, 90)
        run('xdotool', 'key', 'Return')
        wait_for(lambda: 'Close or save' in status(), 'second logout started')
        click(session, 124, 230)
        run('xdotool', 'key', 'Escape')
        assert wm.poll() is None
        click(session, 124, 230)
        run('xdotool', 'key', 'Return')
        wm.wait(timeout=5)
    finally:
        stop(wm)
        for child in children:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)

    wm = start(1, '/bin/false')
    try:
        run('xdotool', 'key', '--clearmodifiers', 'alt+F1')
        wait_for(lambda: windows('^WiiDesk Launcher$'), 'launcher before lock')
        run('xdotool', 'key', '--clearmodifiers', 'ctrl+alt+l')
        wm.wait(timeout=5)
    finally:
        stop(wm)
    print('PASS: unmanaged logout guard, locker completion, dirty-editor logout/cancel/force, locker failure ends managed session')


if __name__ == '__main__':
    main()
