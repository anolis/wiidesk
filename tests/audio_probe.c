// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "audio_decode.h"
#include "audio_playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    if(argc<3)return 2;
    if(!strcmp(argv[1],"list")) {
        struct audio_playlist *list=calloc(1,sizeof(*list)); if(!list)return 2;
        audio_playlist_load(argv[2],list);
        printf("%s %d %s\n",list->ok?"OK":"ERROR",list->count,list->error);
        if(list->ok)for(int i=0;i<list->count;i++)puts(list->paths[i]);
        int rc=list->ok?0:1; free(list); return rc;
    }
    struct audio_info info; char error[200];
    struct audio_decoder *d=audio_open(argv[2],&info,error,sizeof(error));
    if(!d) { printf("ERROR %s\n",error); return 1; }
    printf("INFO %u %u %lld %d\n",info.rate,info.channels,(long long)info.frames,info.estimated);
    if(argc==4 && audio_seek(d,strtoll(argv[3],NULL,10))<0) { puts(audio_error(d)); audio_close(d); return 1; }
    int16_t samples[8192]; int n; long long frames=0,sum=0;
    while((n=audio_read(d,samples,4096))>0) {
        if(!frames) { printf("FIRST"); for(int i=0;i<n*(int)info.channels && i<8;i++)printf(" %d",samples[i]); puts(""); }
        frames+=n; for(int i=0;i<n*(int)info.channels;i++)sum+=samples[i];
    }
    printf("RESULT %lld %lld\n",frames,sum);
    if(n<0)puts(audio_error(d));
    audio_close(d); return n<0?1:0;
}
