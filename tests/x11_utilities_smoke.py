#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise utility windows through the launcher and ordinary input events."""
import os
import pathlib
import time
import socket
from x11_desktop_smoke import run, launch, close, wait_for
from x11_core_apps_smoke import keys, type_text


def status(window):
    return run('xprop', '-id', window, '_WIIDESK_UTILITY_STATUS')


def main():
    calc = launch(8, '^WiiDesk Calculator$')
    type_text('sqrt(81)+2^3')
    keys('Return')
    wait_for(lambda: '"= 17"' in status(calc), 'calculator evaluates expression')
    keys('ctrl+a')
    type_text('1/0')
    keys('Return')
    wait_for(lambda: 'Division by zero' in status(calc), 'calculator reports error')
    keys('Up', 'Return')
    wait_for(lambda: '"= 17"' in status(calc), 'history recalls valid expression')
    close(calc)

    home = pathlib.Path(os.environ['WIIDESK_TEST_HOME'])
    log = home / 'utility.log'
    log.write_text('alpha\nbeta\nneedle\n')
    viewer = launch(7, '^WiiDesk Logs$')
    keys('ctrl+a')
    type_text(str(log))
    keys('Return')
    wait_for(lambda: '3 matching / 3 lines' in status(viewer), 'log opens')
    keys('Tab')
    type_text('needle')
    wait_for(lambda: '1 matching / 3 lines' in status(viewer), 'filter matches')
    with log.open('a') as f:
        f.write('needle appended\n')
    wait_for(lambda: '2 matching / 4 lines' in status(viewer), 'log follows append')
    keys('F6')
    with log.open('a') as f:
        f.write('needle while paused\n')
    time.sleep(1.3)
    assert '2 matching / 4 lines' in status(viewer)
    keys('F6')
    wait_for(lambda: '3 matching / 5 lines' in status(viewer), 'follow resumes after pause')
    # A new inode at the same path must replace the old display.
    log.rename(home / 'utility.log.1')
    log.write_text('fresh needle\n')
    wait_for(lambda: '1 matching / 1 lines' in status(viewer), 'log follows rotation')
    log.write_text('short\n')
    wait_for(lambda: '0 matching / 1 lines' in status(viewer), 'log follows truncation')
    log.write_text('needle discarded\n' + '\n' * 60000 + 'needle retained\n')
    wait_for(lambda: '1 matching / 512 lines' in status(viewer), 'short-line tail stays bounded and retains newest lines')
    close(viewer)
    # Use a local listener so the connectivity test never depends on Internet.
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    network = launch(9, '^WiiDesk Network$')
    wait_for(lambda: 'Status refreshed' in status(network), 'network status loads')
    keys('ctrl+a')
    type_text(f'127.0.0.1:{listener.getsockname()[1]}')
    keys('Return')
    wait_for(lambda: 'connection succeeded' in status(network), 'TCP check succeeds')
    listener.close()
    keys('F2')
    type_text('test-only')
    keys('Tab')
    type_text('short')
    keys('Return')
    wait_for(lambda: '8-63 ASCII' in status(network), 'short Wi-Fi password rejected without changing a connection')
    close(network)
    print('PASS: calculator input/history/errors; bounded log follow/filter/rotation; network status/TCP/validation')


if __name__ == '__main__':
    main()
