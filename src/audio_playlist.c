// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "audio_playlist.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
static int suffix(const char *p,const char *s)
{ size_t n=strlen(p),m=strlen(s); return n>=m && !strcasecmp(p+n-m,s); }
static int supported(const char *p) { return suffix(p,".wav") || suffix(p,".mp3"); }
static int add(struct audio_playlist *list,const char *path)
{
    if(list->count==AUDIO_TRACKS) { strcpy(list->error,"Playlist limit: 128 tracks"); return -1; }
    char resolved[PATH_MAX]; struct stat st;
    for(const unsigned char *p=(const unsigned char *)path;*p;p++)if(*p<32 || *p==127)goto invalid;
    if(!realpath(path,resolved) || !supported(resolved) || stat(resolved,&st) || !S_ISREG(st.st_mode) || st.st_size<12 || st.st_size>1024LL*1024*1024)goto invalid;
    strcpy(list->paths[list->count++],resolved); return 0;
invalid:
    strcpy(list->error,"Track unavailable: regular WAV/MP3 files up to 1 GiB only"); return -1;
}
static int compare(const void *a,const void *b) { return strcmp(a,b); }
void audio_playlist_load(const char *path,struct audio_playlist *list)
{
    memset(list,0,sizeof(*list));
    char full[PATH_MAX],item[PATH_MAX]; struct stat st;
    if(!realpath(path,full) || stat(full,&st)) { strcpy(list->error,"Cannot open audio path"); return; }
    if(S_ISDIR(st.st_mode)) {
        DIR *dir=opendir(full); if(!dir) { strcpy(list->error,"Cannot read folder"); return; }
        struct dirent *de; unsigned scanned=0;
        while((de=readdir(dir))) {
            if(++scanned>4096) { strcpy(list->error,"Folder limit: 4096 entries"); break; }
            if(!supported(de->d_name))continue;
            if(snprintf(item,sizeof(item),"%s/%s",full,de->d_name)>=(int)sizeof(item)) { strcpy(list->error,"Track path too long"); break; }
            if(add(list,item))break;
        }
        closedir(dir); if(list->error[0])return;
        qsort(list->paths,(size_t)list->count,sizeof(list->paths[0]),compare);
    } else if(suffix(full,".m3u") || suffix(full,".m3u8")) {
        int fd=open(full,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>65536) {
            if(fd>=0)close(fd);
            strcpy(list->error,"Playlist must be a regular file up to 64 KiB"); return;
        }
        char *text=malloc(65538); if(!text) { close(fd); strcpy(list->error,"Cannot read playlist"); return; }
        size_t bytes=0;
        while(bytes<65537) {
            ssize_t n=read(fd,text+bytes,65537-bytes);
            if(n<0 && errno==EINTR)continue;
            if(n<0) { strcpy(list->error,"Playlist read failed"); break; }
            if(!n)break;
            bytes+=(size_t)n;
        }
        close(fd);
        if(bytes>65536 || memchr(text,0,bytes))strcpy(list->error,"Playlist must be text up to 64 KiB");
        if(list->error[0]) { free(text); return; }
        text[bytes]=0;
        char *slash=strrchr(full,'/'); if(slash)*slash=0;
        char *cursor=text,*line;
        if(bytes>=3 && !memcmp(text,"\xef\xbb\xbf",3))cursor+=3;
        while((line=strsep(&cursor,"\n"))) {
            size_t n=strlen(line);
            if(n && line[n-1]=='\r')line[--n]=0;
            if(!line[0] || line[0]=='#')continue;
            if(n>=PATH_MAX) { strcpy(list->error,"Track path too long"); break; }
            if(line[0]=='/')snprintf(item,sizeof(item),"%s",line);
            else if(snprintf(item,sizeof(item),"%s/%s",full,line)>=(int)sizeof(item)) { strcpy(list->error,"Track path too long"); break; }
            if(add(list,item))break;
        }
        free(text); if(list->error[0])return;
    } else if(add(list,full))return;
    if(!list->count) { strcpy(list->error,"No WAV or MP3 tracks found"); return; }
    list->ok=1;
}
