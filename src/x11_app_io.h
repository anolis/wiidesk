/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_APP_IO_H
#define WIIDESK_APP_IO_H
#include <sys/stat.h>
#include <stddef.h>

struct document_stamp { int exists; struct stat st; };
int document_read(const char *, char *, size_t capacity, size_t *, struct document_stamp *);
int document_write(const char *, const char *, size_t, const struct document_stamp *, struct document_stamp *);
int move_new(const char *, const char *);
struct file_copy {
    int input, output;
    off_t total, done;
    struct stat source_stamp;
    char temporary[4096], destination[4096];
};
int copy_start(struct file_copy *, const char *, const char *);
int copy_step(struct file_copy *); /* 0 pending, 1 complete, -1 failed */
void copy_cancel(struct file_copy *);
int trash_move(const char *, char *saved, size_t, char *info, size_t);
#endif
