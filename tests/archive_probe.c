// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "archive_io.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    if(argc<2 || argc>3)return 2;
    int fd=argc==3?open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW):-1;
    if(argc==3 && fd<0)return 2;
    struct archive_result *r=calloc(1,sizeof(*r)); if(!r)return 2;
    volatile sig_atomic_t cancelled=0;
    archive_process(argv[1],fd,r,&cancelled);
    printf("%s entries=%u bytes=%u %s\n",r->ok?"OK":"ERROR",atomic_load(&r->count),atomic_load(&r->bytes),r->error);
    for(unsigned i=0;i<atomic_load(&r->count);i++)printf("%c %u %s\n",r->rows[i].directory?'D':'F',r->rows[i].size,r->rows[i].name);
    int rc=!r->ok; free(r); if(fd>=0)close(fd); return rc;
}
