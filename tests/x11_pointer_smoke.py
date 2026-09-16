#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run against WiiDesk in a disposable X server (requires xterm/xdotool/xwininfo)."""
import os
import pathlib
import re
import subprocess
import tempfile
import time


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def geometry(window):
    info = run('xwininfo', '-id', window)
    return tuple(int(re.search(rf'{label}:\s+(-?\d+)', info)[1]) for label in
                 ('Absolute upper-left X', 'Absolute upper-left Y', 'Width', 'Height'))


def main():
    assert os.environ.get('DISPLAY'), 'Set DISPLAY to a disposable test server'
    title = f'WiiDesk pointer test {os.getpid()}'
    with tempfile.TemporaryDirectory(prefix='wiidesk-input-') as directory:
        output = pathlib.Path(directory) / 'input.txt'
        terminal = subprocess.Popen([
            'xterm', '-fn', '6x13', '-title', title, '-geometry', '40x10+30+30',
            '-e', 'sh', '-c', 'IFS= read -r line; printf "%s" "$line" > "$1"; sleep 60',
            'sh', str(output)])
        try:
            for _ in range(100):
                found = subprocess.run(['xdotool', 'search', '--name', f'^{title}$'],
                                       capture_output=True, text=True)
                if found.returncode == 0:
                    break
                time.sleep(.05)
            assert found.returncode == 0, 'terminal appeared'
            window = found.stdout.strip().splitlines()[0]
            time.sleep(.3)
            old = geometry(window)
            x, y = old[:2]
            # No sleeps between press/motion/release: cover events queued before grab.
            run('xdotool', 'mousemove', str(x + 30), str(y - 12), 'mousedown', '1',
                'mousemove', str(x + 90), str(y + 28), 'mouseup', '1')
            time.sleep(.3)
            moved = geometry(window)
            assert moved[:2] == (x + 60, y + 40), (old, moved)
            x, y, width, height = moved
            run('xdotool', 'mousemove', str(x + width + 1), str(y + height + 1),
                'mousedown', '1', 'mousemove', str(x + width + 31),
                str(y + height + 21), 'mouseup', '1')
            time.sleep(.3)
            resized = geometry(window)
            assert resized[2] > width and resized[3] > height, (moved, resized)
            run('xdotool', 'mousemove', str(x + 20), str(y + 20), 'click', '1',
                'type', '--clearmodifiers', 'wiidesk-input-ok')
            run('xdotool', 'key', 'Return')
            for _ in range(100):
                if output.exists():
                    break
                time.sleep(.05)
            assert output.read_text() == 'wiidesk-input-ok'
            print('PASS: fast title drag, resize grip, click focus, terminal keyboard input')
            print('geometry:', old, moved, resized)
        finally:
            terminal.terminate()
            terminal.wait(timeout=5)


if __name__ == '__main__':
    main()
