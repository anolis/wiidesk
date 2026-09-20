/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_ARCHIVE_IO_H
#define WIIDESK_ARCHIVE_IO_H
#include <stdatomic.h>
#include <signal.h>
#define ARCHIVE_ENTRIES 512
#define ARCHIVE_BYTES (64u*1024u*1024u)
#define ARCHIVE_PARTIAL ".wiidesk-incomplete"
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Archive progress needs lock-free shared counters");
struct archive_row { char name[512]; unsigned size; int directory; };
struct archive_result {
    atomic_uint count, bytes, input, input_size;
    int ok;
    char error[192];
    struct archive_row rows[ARCHIVE_ENTRIES];
};
/* destination is an already-open private directory, or -1 for inspection. */
void archive_process(const char *,int,struct archive_result *,volatile sig_atomic_t *);
#endif
