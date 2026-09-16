/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_X11_PREFERENCES_H
#define WIIDESK_X11_PREFERENCES_H
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct preferences { unsigned int background, accent, idle_lock_seconds; };
static const unsigned int idle_lock_choices[] = {0, 60, 300, 600, 900, 1800};
static const char *const background_names[] = { "Aqua", "Sunset", "Graphite" };
static const char *const background_colors[] = { "#244b59", "#593b42", "#292f33" };
static const char *const accent_names[] = { "Teal", "Sky", "Violet" };
static const char *const accent_colors[] = { "#007f78", "#245e91", "#69488a" };
#define SETTINGS_MESSAGE "_WIIDESK_RELOAD_SETTINGS"

static inline int preferences_path(char *path, size_t size, int create)
{
    char base[PATH_MAX], directory[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int n;
    if (xdg && *xdg == '/') n = snprintf(base, sizeof(base), "%s", xdg);
    else if (home && *home == '/') n = snprintf(base, sizeof(base), "%s/.config", home);
    else { errno = ENOENT; return -1; }
    if (n >= (int)sizeof(base) ||
        snprintf(directory, sizeof(directory), "%s/wiidesk", base) >= (int)sizeof(directory) ||
        snprintf(path, size, "%s/x11.conf", directory) >= (int)size) {
        errno = ENAMETOOLONG; return -1;
    }
    if (create && ((mkdir(base, 0700) && errno != EEXIST) ||
                   (mkdir(directory, 0700) && errno != EEXIST))) return -1;
    return 0;
}
static inline void preferences_load(struct preferences *p)
{
    char path[PATH_MAX], line[128];
    *p = (struct preferences){0, 0, 300};
    if (preferences_path(path, sizeof(path), 0)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned int value;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "background=%u", &value) == 1 && value < 3) p->background = value;
        if (sscanf(line, "accent=%u", &value) == 1 && value < 3) p->accent = value;
        if (sscanf(line, "idle_lock_seconds=%u", &value) == 1 && value <= 86400) p->idle_lock_seconds = value;
    }
    fclose(f);
}
static inline int preferences_save(const struct preferences *p)
{
    char path[PATH_MAX], temporary[PATH_MAX];
    if (preferences_path(path, sizeof(path), 1)) return -1;
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) {
        errno = ENAMETOOLONG; return -1;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { int saved = errno; close(fd); unlink(temporary); errno = saved; return -1; }
    int failed = fprintf(f, "background=%u\naccent=%u\nidle_lock_seconds=%u\n", p->background, p->accent, p->idle_lock_seconds) < 0;
    if (fflush(f) || fsync(fd)) failed = 1;
    if (fclose(f)) failed = 1;
    if (!failed && !rename(temporary, path)) return 0;
    int saved = errno; unlink(temporary); errno = saved ? saved : EIO; return -1;
}
#endif
