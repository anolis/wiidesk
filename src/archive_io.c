// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "archive_io.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int normalize(const char *raw,char *out,int directory)
{
    if(!raw || !*raw || *raw=='/' || strlen(raw)>511 || (raw[0] && raw[1]==':'))return -1;
    for(const unsigned char *p=(const unsigned char *)raw;*p;p++)if(*p<32 || *p==127 || *p=='\\')return -1;
    char copy[512]; strcpy(copy,raw); char *save,*part=strtok_r(copy,"/",&save);
    unsigned depth=0; out[0]=0;
    while(part) {
        if(!strcmp(part,".."))return -1;
        if(strcmp(part,".")) {
            if(++depth>16 || strlen(part)>255)return -1;
            if(out[0])strcat(out,"/");
            strcat(out,part);
        }
        part=strtok_r(NULL,"/",&save);
    }
    return !out[0] && !directory?-1:0;
}
static int output_file(int root,const char *path,int directory,unsigned *folders)
{
    char copy[512]; strcpy(copy,path);
    int fd=dup(root); if(fd<0)return -1;
    char *save,*part=strtok_r(copy,"/",&save);
    while(part) {
        char *next=strtok_r(NULL,"/",&save);
        int child;
        if(next || directory) {
            if(!mkdirat(fd,part,0700)) {
                if(++*folders>512) { errno=EFBIG; close(fd); return -1; }
            } else if(errno!=EEXIST) { close(fd); return -1; }
            child=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        } else child=openat(fd,part,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        int saved=errno; close(fd); errno=saved;
        if(child<0)return -1;
        fd=child; part=next;
    }
    return fd;
}
void archive_process(const char *path,int destination,struct archive_result *r,volatile sig_atomic_t *cancelled)
{
    struct archive *a=NULL; int fd=-1,out=-1; unsigned folders=0,count=0,total=0;
    r->ok=0; r->error[0]=0;
    atomic_store(&r->count,0); atomic_store(&r->bytes,0); atomic_store(&r->input,0); atomic_store(&r->input_size,0);
    fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    if(fd<0)goto system_error;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>32*1024*1024) {
        strcpy(r->error,"Choose a regular archive, at most 32 MiB"); goto done;
    }
    atomic_store(&r->input_size,(unsigned)st.st_size);
    a=archive_read_new(); if(!a) { strcpy(r->error,"Not enough decoder memory"); goto done; }
    /* No external filter helpers: only linked-in codecs and ZIP/tar formats. */
    if(archive_read_support_filter_none(a)!=ARCHIVE_OK || archive_read_support_filter_gzip(a)!=ARCHIVE_OK ||
       archive_read_support_filter_bzip2(a)!=ARCHIVE_OK || archive_read_support_filter_xz(a)!=ARCHIVE_OK) {
        strcpy(r->error,"Required built-in archive codecs unavailable"); goto done;
    }
    archive_read_support_format_tar(a); archive_read_support_format_zip(a);
    if(archive_read_open_fd(a,fd,16384)!=ARCHIVE_OK)goto archive_error;
    struct archive_entry *entry; int rc;
    while((rc=archive_read_next_header(a,&entry))==ARCHIVE_OK) {
        if(*cancelled) { strcpy(r->error,"Cancelled"); goto done; }
        if(count==ARCHIVE_ENTRIES) { strcpy(r->error,"Archive limit: 512 entries"); goto done; }
        int directory=archive_entry_filetype(entry)==AE_IFDIR;
        if((!directory && archive_entry_filetype(entry)!=AE_IFREG) || archive_entry_hardlink(entry) || archive_entry_symlink(entry) || archive_entry_is_encrypted(entry)) {
            strcpy(r->error,"Only unencrypted regular files and directories are supported"); goto done;
        }
        char name[512];
        if(normalize(archive_entry_pathname(entry),name,directory)) { strcpy(r->error,"Unsafe or overlong archive path"); goto done; }
        if(!strcmp(name,ARCHIVE_PARTIAL)) { strcpy(r->error,"Reserved extraction marker path"); goto done; }
        /* A tar root directory is harmless, but still counts toward the limit. */
        for(unsigned i=0;i<count;i++)if(!strcmp(name,r->rows[i].name)) {
            strcpy(r->error,"Duplicate archive path"); goto done;
        }
        la_int64_t declared=archive_entry_size(entry);
        if(declared<0 || declared>32*1024*1024 || (unsigned)declared>ARCHIVE_BYTES-total) {
            strcpy(r->error,"Size limit: 32 MiB/file, 64 MiB total"); goto done;
        }
        struct archive_row *row=&r->rows[count];
        strcpy(row->name,name[0]?name:"."); row->directory=directory; row->size=(unsigned)declared;
        count++; atomic_store(&r->count,count);
        if(destination>=0 && name[0]) {
            out=output_file(destination,name,directory,&folders);
            if(out<0)goto system_error;
        }
        unsigned used=0; char buffer[16384]; la_ssize_t n;
        while((n=archive_read_data(a,buffer,sizeof(buffer)))>0) {
            if(*cancelled) { strcpy(r->error,"Cancelled"); goto done; }
            if(directory || (unsigned)n>32u*1024u*1024u-used || (unsigned)n>ARCHIVE_BYTES-total) {
                strcpy(r->error,"Size limit exceeded by decoded data"); goto done;
            }
            if(out>=0) {
                size_t written=0;
                while(written<(size_t)n) {
                    ssize_t k=write(out,buffer+written,(size_t)n-written);
                    if(k<0 && errno==EINTR && !*cancelled)continue;
                    if(k<=0)goto system_error;
                    written+=(size_t)k;
                }
            }
            used+=(unsigned)n; total+=(unsigned)n; atomic_store(&r->bytes,total);
            la_int64_t read=archive_filter_bytes(a,-1);
            if(read>=0)atomic_store(&r->input,(unsigned)read);
        }
        if(n<0)goto archive_error;
        if(!directory && archive_entry_size_is_set(entry) && used!=(unsigned)declared) {
            strcpy(r->error,"Incomplete archive entry"); goto done;
        }
        if(out>=0) { int k=close(out); out=-1; if(k)goto system_error; }
    }
    if(rc!=ARCHIVE_EOF)goto archive_error;
    if(*cancelled) { strcpy(r->error,"Cancelled"); goto done; }
    r->ok=1; atomic_store(&r->input,(unsigned)st.st_size); goto done;
archive_error:
    snprintf(r->error,sizeof(r->error),"Archive: %.175s",archive_error_string(a)?archive_error_string(a):"decode failed"); goto done;
system_error:
    snprintf(r->error,sizeof(r->error),"File operation: %s",strerror(errno));
done:
    if(out>=0)close(out);
    if(a)archive_read_free(a);
    if(fd>=0)close(fd);
}
