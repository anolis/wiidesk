#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual GUI actions against disposable files and our own child process."""
import os
import pathlib
import subprocess
import time
from x11_desktop_smoke import run, wait_for, windows, launch, close


def keys(*names):
    run('xdotool', 'key', '--clearmodifiers', *names)


def type_text(value):
    run('xdotool', 'type', '--clearmodifiers', '--', str(value))


def dialog(value):
    keys('ctrl+a')
    type_text(value)
    keys('Return')


def save_as(path):
    keys('ctrl+shift+s')
    dialog(path)
    wait_for(path.exists, 'editor saved new file')


def main():
    home = pathlib.Path(os.environ['WIIDESK_TEST_HOME'])
    assert str(home) == os.environ['HOME'] and home.name == 'home'
    editor = launch(4, '^WiiDesk Editor$')
    type_text('hello world')
    first = home / 'editor-one.txt'
    save_as(first)
    assert first.read_text() == 'hello world'
    keys('ctrl+a', 'ctrl+c')
    # A different process must obtain the X11 selection, not a local buffer.
    app = pathlib.Path(os.environ['WIIDESK_TEST_BUILD']) / 'wiidesk-x11-app'
    second_app = subprocess.Popen([str(app), 'editor'])
    wait_for(lambda: len(windows('^WiiDesk Editor$')) == 2, 'second editor mapped')
    second = next(w for w in windows('^WiiDesk Editor$') if w != editor)
    run('xdotool', 'windowactivate', '--sync', second)
    keys('ctrl+v')
    time.sleep(.15)
    second_path = home / 'editor-two.txt'
    save_as(second_path)
    assert second_path.read_text() == 'hello world'
    keys('ctrl+f')
    dialog('world')
    type_text('Wii')
    keys('ctrl+s')
    wait_for(lambda: second_path.read_text() == 'hello Wii', 'find/replace selection saves')
    keys('ctrl+z', 'ctrl+s')
    wait_for(lambda: second_path.read_text() == 'hello world', 'undo restores content')
    # External changes must survive a stale editor save.
    second_path.write_text('external edit')
    keys('ctrl+End')
    type_text('!')
    keys('ctrl+s')
    time.sleep(.15)
    assert second_path.read_text() == 'external edit'
    # Closing dirty text prompts; Escape cancels that prompt.
    keys('alt+F4', 'Escape')
    assert second in windows('^WiiDesk Editor$')
    keys('alt+F4', 'Return')
    wait_for(lambda: second not in windows('^WiiDesk Editor$'), 'confirmed discard closes')
    second_app.wait(timeout=5)
    run('xdotool', 'windowactivate', '--sync', editor)
    # Over-limit input is rejected without replacing the currently loaded buffer.
    large = home / 'too-large.txt'
    large.write_bytes(b'x' * 262145)
    keys('ctrl+o')
    dialog(large)
    keys('ctrl+s')
    time.sleep(.15)
    assert first.read_text() == 'hello world'
    close(editor)

    folder = home / 'file-operations'
    folder.mkdir()
    original = folder / 'a.txt'
    original.write_text('copy me')
    files = launch(1, '^WiiDesk Files')
    keys('ctrl+l')
    dialog(folder)
    wait_for(lambda: run('xdotool', 'getwindowname', files).endswith(str(folder)), 'folder opened')
    keys('F8')
    dialog('b.txt')
    copy = folder / 'b.txt'
    wait_for(copy.exists, 'copy completed')
    assert copy.read_text() == 'copy me'
    keys('F2')
    dialog('b.txt')
    time.sleep(.15)
    assert original.read_text() == 'copy me' and copy.read_text() == 'copy me'
    keys('F2')
    dialog('c.txt')
    renamed = folder / 'c.txt'
    wait_for(renamed.exists, 'rename completed')
    assert not original.exists()
    keys('F6')
    moved = home / 'moved.txt'
    dialog(moved)
    wait_for(moved.exists, 'move completed')
    assert not copy.exists()
    keys('Delete', 'Return')
    wait_for(lambda: not renamed.exists(), 'trash move completed')
    keys('ctrl+z')
    wait_for(renamed.exists, 'last trash restored')
    assert renamed.read_text() == 'copy me'
    keys('F7')
    dialog('new-folder')
    wait_for(lambda: (folder / 'new-folder').is_dir(), 'new folder created')
    close(files)

    # Find and terminate only the sleep process created by this test. Explicit
    # PID filtering also works when a busy host exceeds the bounded 512-row list.
    # It must exit because of our TERM request, even on a slow test host.
    victim = subprocess.Popen(['sleep', '86400'])
    try:
        processes = launch(5, '^WiiDesk Processes$')
        keys('ctrl+f')
        dialog(str(victim.pid))
        wait_for(lambda: run('xprop', '-id', processes, '_WIIDESK_SELECTED_PID').endswith(f'= {victim.pid}'), 'own process selected by PID')
        keys('Delete', 'Escape')
        assert victim.poll() is None
        keys('Delete', 'Return')
        wait_for(lambda: victim.poll() is not None, 'confirmed TERM reaches selected child')
        close(processes)
    finally:
        if victim.poll() is None:
            victim.terminate()
        victim.wait(timeout=5)
    print('PASS: editor dialogs/save/find/undo/clipboard/stale-file/close guards; copy/rename/move/trash/restore/mkdir; process confirmation and TERM')


if __name__ == '__main__':
    main()
