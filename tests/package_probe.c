// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "package_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    struct package_result *r=calloc(1,sizeof(*r)); if(!r)return 2;
    if(strlen(argv[2])>=sizeof(r->spool))return 2;
    strcpy(r->spool,argv[2]); signal(SIGTERM,package_cancel);
    package_inspect(argv[1],r);
    printf("%s: %s\n",r->ok?"OK":"FAIL",r->error);
    for(unsigned i=0;i<atomic_load(&r->count);i++) {
        struct package_entry *e=&r->entries[i];
        printf("%s %s %s %s\n",e->name,e->version,e->arch,e->digest);
    }
    int rc=r->ok?0:1; free(r); return rc;
}
