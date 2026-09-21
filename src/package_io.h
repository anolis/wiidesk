/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_PACKAGE_IO_H
#define WIIDESK_PACKAGE_IO_H
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/types.h>
#define PACKAGE_MAX 16
#define PACKAGE_FILE_MAX (32u*1024u*1024u)
#define PACKAGE_TOTAL_MAX (64u*1024u*1024u)
struct package_entry {
    char file[PATH_MAX], digest[65], name[128], version[128], arch[32];
    char depends[1024], predepends[512], description[256], installed[128];
    unsigned size, installed_kib;
};
struct package_result {
    atomic_uint count;
    int ok;
    char error[256], spool[PATH_MAX];
    struct package_entry entries[PACKAGE_MAX];
};
int package_capture(char *const [],char *,size_t,int);
int package_hash(const char *,char [65]);
int package_metadata(const char *,struct package_entry *,char *,size_t);
int package_copy(const char *,int,uid_t,unsigned,unsigned *,char *,size_t);
void package_inspect(const char *,struct package_result *);
void package_cancel(int);
#endif
