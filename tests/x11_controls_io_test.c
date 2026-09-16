// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "x11_controls.h"
#include "x11_app_io.h"
#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    struct ui_text text;
    assert(!ui_text_init(&text, 16));
    assert(!ui_text_set(&text, "first\nsecond", 12));
    text.cursor = text.anchor = 12;
    assert(ui_text_key(&text, XK_Up, 0, "", 0, 1) == 1 && text.cursor == 5);
    text.anchor = 0;
    assert(!ui_text_insert(&text, "new", 3) && !strcmp(text.data, "new\nsecond"));
    assert(ui_text_key(&text, XK_z, ControlMask, "", 0, 1) == 1 && !strcmp(text.data, "first\nsecond"));
    assert(ui_text_insert(&text, "abcdefghijklmnopq", 17) == -1);
    assert(!strcmp(text.data, "first\nsecond"));
    assert(ui_text_set(&text, "\xff", 1) == -1 && !strcmp(text.data, "first\nsecond"));
    ui_text_free(&text);

    char dir[] = "/tmp/wiidesk-io-test.XXXXXX"; assert(mkdtemp(dir));
    char file[4096], copy[4096], moved[4096], linkpath[4096], fifo[4096];
    snprintf(file, sizeof(file), "%s/document", dir); snprintf(copy, sizeof(copy), "%s/copy", dir);
    snprintf(moved, sizeof(moved), "%s/moved", dir); snprintf(linkpath, sizeof(linkpath), "%s/symlink", dir);
    snprintf(fifo, sizeof(fifo), "%s/fifo", dir);
    struct document_stamp empty = {0}, stamp, after;
    assert(!document_write(file, "original", 8, &empty, &stamp));
    assert(!chmod(file, 0640));
    char buf[65]; size_t n;
    assert(!document_read(file, buf, 64, &n, &stamp) && n == 8 && !strcmp(buf, "original"));
    assert(document_write(file, "overwrite", 9, &empty, &after) == -1 && errno == EEXIST);
    assert(!document_write(file, "saved", 5, &stamp, &after));
    assert((after.st.st_mode & 0777) == 0640);
    assert(document_write(file, "stale", 5, &stamp, &after) == -1);
    assert(document_read(file, buf, 4, &n, &stamp) == -1 && errno == EFBIG);
    assert(!symlink(file, linkpath));
    assert(document_read(linkpath, buf, 64, &n, &stamp) == -1);
    assert(!mkfifo(fifo, 0600));
    assert(document_read(fifo, buf, 64, &n, &stamp) == -1);
    struct file_copy job;
    assert(!copy_start(&job, file, copy));
    int result; while (!(result = copy_step(&job))) {}
    assert(result == 1);
    assert(!document_read(copy, buf, 64, &n, &stamp) && !strcmp(buf, "saved"));
    assert(copy_start(&job, file, copy) == -1 && errno == EEXIST);
    assert(move_new(file, copy) == -1 && errno == EEXIST);
    assert(!move_new(copy, moved));
    assert(!copy_start(&job, file, copy));
    char temporary[4096]; snprintf(temporary, sizeof(temporary), "%s", job.temporary);
    copy_cancel(&job); assert(access(copy, F_OK) && access(temporary, F_OK));
    assert(!copy_start(&job, file, copy));
    int fd = open(file, O_WRONLY | O_APPEND); assert(fd >= 0);
    assert(write(fd, "changed", 7) == 7); close(fd);
    while (!(result = copy_step(&job))) {}
    assert(result == -1 && errno == ESTALE && access(copy, F_OK));
    unlink(file); unlink(moved); unlink(linkpath); unlink(fifo); assert(!rmdir(dir));
    puts("PASS: text bounds/selection/undo, exact file limits, stale-save protection, permissions, symlink/FIFO rejection, copy/move no-overwrite, cancellation");
    return 0;
}
