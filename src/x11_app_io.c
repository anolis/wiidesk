// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "x11_app_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/xattr.h>

static int same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int write_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; size -= n;
    }
    return 0;
}
int document_read(const char *path, char *data, size_t capacity, size_t *length, struct document_stamp *stamp)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st; int saved = 0; size_t size = 0;
    if (fstat(fd, &st)) saved = errno;
    else if (!S_ISREG(st.st_mode)) saved = EINVAL;
    else if (st.st_size > (off_t)capacity) saved = EFBIG;
    while (!saved && size <= capacity) {
        char extra;
        ssize_t n = read(fd, size < capacity ? data + size : &extra, size < capacity ? capacity - size : 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { saved = errno; break; }
        if (!n) break;
        if (size == capacity) { saved = EFBIG; break; }
        size += n;
    }
    struct stat after;
    if (!saved && (fstat(fd, &after) || !same_file(&st, &after))) saved = ESTALE;
    close(fd);
    if (saved) { errno = saved; return -1; }
    data[size] = 0; *length = size; stamp->exists = 1; stamp->st = st; return 0;
}
int move_new(const char *from, const char *to)
{ return renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE); }
int document_write(const char *path, const char *data, size_t size,
                   const struct document_stamp *expected, struct document_stamp *result)
{
    struct stat current; int exists = !lstat(path, &current);
    if (!exists && errno != ENOENT) return -1;
    if (exists && (!expected->exists || !same_file(&current, &expected->st))) { errno = EEXIST; return -1; }
    if (expected->exists && !exists) { errno = ESTALE; return -1; }
    /* Do not replace symlinks, devices or hard-linked files. */
    if (exists && (!S_ISREG(current.st_mode) || current.st_nlink != 1 || current.st_uid != geteuid())) { errno = EPERM; return -1; }
    if (exists && access(path, W_OK)) return -1;
    if (exists) {
        ssize_t attributes = listxattr(path, NULL, 0);
        if (attributes > 0) { errno = ENOTSUP; return -1; }
        if (attributes < 0 && errno != ENOTSUP) return -1;
    }
    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.wiidesk-XXXXXX", path) >= (int)sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(temporary); if (fd < 0) return -1;
    int error = 0;
    if (exists && (fchown(fd, current.st_uid, current.st_gid) || fchmod(fd, current.st_mode & 0777))) error = errno;
    if (!error && (write_all(fd, data, size) || fsync(fd))) error = errno;
    if (close(fd) && !error) error = errno;
    if (!error && exists) {
        struct stat check;
        if (lstat(path, &check) || !same_file(&check, &current)) error = ESTALE;
    }
    if (!error && (exists ? rename(temporary, path) : move_new(temporary, path))) error = errno;
    if (error) { unlink(temporary); errno = error; return -1; }
    result->exists = 1;
    return stat(path, &result->st);
}
void copy_cancel(struct file_copy *c)
{
    int saved = errno;
    if (c->input >= 0) close(c->input);
    if (c->output >= 0) close(c->output);
    if (c->temporary[0]) unlink(c->temporary);
    c->input = c->output = -1; c->temporary[0] = 0; errno = saved;
}
int copy_start(struct file_copy *c, const char *from, const char *to)
{
    *c = (struct file_copy){ .input = -1, .output = -1 };
    if (snprintf(c->destination, sizeof(c->destination), "%s", to) >= (int)sizeof(c->destination) ||
        snprintf(c->temporary, sizeof(c->temporary), "%s.wiidesk-XXXXXX", to) >= (int)sizeof(c->temporary)) {
        c->temporary[0] = 0; errno = ENAMETOOLONG; return -1;
    }
    struct stat st;
    if (!lstat(to, &st)) { c->temporary[0] = 0; errno = EEXIST; return -1; }
    if (errno != ENOENT) { c->temporary[0] = 0; return -1; }
    c->input = open(from, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (c->input < 0) { c->temporary[0] = 0; return -1; }
    if (fstat(c->input, &st) || !S_ISREG(st.st_mode)) {
        c->temporary[0] = 0; errno = EINVAL; copy_cancel(c); return -1;
    }
    c->total = st.st_size;
    c->source_stamp = st;
    c->output = mkstemp(c->temporary);
    if (c->output < 0) { c->temporary[0] = 0; copy_cancel(c); return -1; }
    if (fchmod(c->output, st.st_mode & 0777)) { copy_cancel(c); return -1; }
    return 0;
}
int copy_step(struct file_copy *c)
{
    char buffer[32768]; ssize_t n = read(c->input, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) return 0;
    if (n < 0 || (n && write_all(c->output, buffer, n))) { copy_cancel(c); return -1; }
    if (n) { c->done += n; return 0; }
    struct stat after;
    if (fstat(c->input, &after) || !same_file(&after, &c->source_stamp)) { errno = ESTALE; copy_cancel(c); return -1; }
    if (fsync(c->output)) { copy_cancel(c); return -1; }
    int fd = c->output; c->output = -1;
    if (close(fd) || move_new(c->temporary, c->destination)) { copy_cancel(c); return -1; }
    c->temporary[0] = 0; copy_cancel(c); return 1;
}
static int private_directory(const char *path)
{
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) { errno = EPERM; return -1; }
    return 0;
}
int trash_move(const char *from, char *saved, size_t saved_size, char *info, size_t info_size)
{
    const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
    char base[4096], path[4096], trash[4096];
    if (xdg && xdg[0] == '/') {
        if (snprintf(base, sizeof(base), "%s", xdg) >= (int)sizeof(base)) { errno = ENAMETOOLONG; return -1; }
    } else {
        if (!home || *home != '/' || snprintf(base, sizeof(base), "%s/.local", home) >= (int)sizeof(base)) { errno = ENAMETOOLONG; return -1; }
        if (private_directory(base)) return -1;
        if (snprintf(base, sizeof(base), "%s/.local/share", home) >= (int)sizeof(base)) { errno = ENAMETOOLONG; return -1; }
    }
    if (private_directory(base)) return -1;
    if (snprintf(trash, sizeof(trash), "%s/Trash", base) >= (int)sizeof(trash)) { errno = ENAMETOOLONG; return -1; }
    if (private_directory(trash)) return -1;
    if (snprintf(path, sizeof(path), "%s/files", trash) >= (int)sizeof(path) || private_directory(path)) return -1;
    if (snprintf(path, sizeof(path), "%s/info", trash) >= (int)sizeof(path) || private_directory(path)) return -1;
    static unsigned serial;
    const char *name = strrchr(from, '/'); name = name ? name + 1 : from;
    char id[256]; snprintf(id, sizeof(id), "%.160s-%ld-%ld-%u", name, (long)time(NULL), (long)getpid(), serial++);
    if (snprintf(saved, saved_size, "%s/files/%s", trash, id) >= (int)saved_size ||
        snprintf(info, info_size, "%s/info/%s.trashinfo", trash, id) >= (int)info_size) { errno = ENAMETOOLONG; return -1; }
    int fd = open(info, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w"); if (!f) { close(fd); unlink(info); return -1; }
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char date[32]; strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
    fputs("[Trash Info]\nPath=", f);
    for (const unsigned char *p = (const unsigned char *)from; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p)) fputc(*p, f);
        else fprintf(f, "%%%02X", *p);
    }
    fprintf(f, "\nDeletionDate=%s\n", date);
    int error = fflush(f) || fsync(fd); if (fclose(f)) error = 1;
    if (!error && !move_new(from, saved)) return 0;
    int saved_errno = errno; unlink(info); errno = saved_errno; return -1;
}
