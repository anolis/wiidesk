// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "audio_decode.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static int64_t audible(snd_pcm_t *pcm,int64_t submitted)
{ snd_pcm_sframes_t delay=0; if(snd_pcm_delay(pcm,&delay)<0 || delay<0)delay=0; return submitted>delay?submitted-delay:0; }
int main(int argc,char **argv)
{
    if(argc!=3 || geteuid()==0) { fprintf(stderr,"Usage (normal user): %s FILE ALSA_DEVICE\n",argv[0]); return 2; }
    prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()==1)return 2;
    struct rlimit memory={48*1024*1024,48*1024*1024},core={0,0};
    if(setrlimit(RLIMIT_AS,&memory) || setrlimit(RLIMIT_CORE,&core))return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    struct audio_info info; char error[200]={0};
    struct audio_decoder *decoder=audio_open(argv[1],&info,error,sizeof(error));
    if(!decoder) { printf("ERROR %s\n",error); return 1; }
    snd_pcm_t *pcm=NULL;
    int rc=snd_pcm_open(&pcm,argv[2],SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK);
    if(rc<0) { printf("ERROR Audio device: %s\n",snd_strerror(rc)); audio_close(decoder); return 1; }
    rc=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16,SND_PCM_ACCESS_RW_INTERLEAVED,info.channels,info.rate,1,100000);
    if(rc<0) { printf("ERROR Audio format: %s\n",snd_strerror(rc)); snd_pcm_close(pcm); audio_close(decoder); return 1; }
    fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);
    printf("INFO %u %u %lld %d\nSTATE playing\n",info.rate,info.channels,(long long)(info.frames<0?-1:info.frames*1000/info.rate),info.estimated);
    int paused=0,volume=50,quit=0,failed=0,eof=0,count=0,offset=0;
    int64_t submitted=0,anchor=0;
    uint64_t began=now(),next=0,progress=now();
    int16_t samples[8192]; char commands[128]; size_t used=0;
    while(!quit) {
        char byte;
        for(unsigned budget=0;budget<256;budget++) {
            ssize_t n=read(STDIN_FILENO,&byte,1);
            if(!n) { quit=1; break; }
            if(n<0) { if(errno==EINTR)continue; if(errno!=EAGAIN)quit=1; break; }
            if(byte!='\n') { if(used>=sizeof(commands)-1) { failed=quit=1; break; } commands[used++]=byte; continue; }
            commands[used]=0; used=0;
            char op,extra; long long value;
            if(!strcmp(commands,"Q")) { quit=1; break; }
            if(sscanf(commands,"%c %lld %c",&op,&value,&extra)!=2)continue;
            if(op=='V') { if(value>=0 && value<=100)volume=(int)value; continue; }
            if((op!='P' && op!='S') || (op=='P' && value!=0 && value!=1) || (op=='S' && (value<0 || value>24*60*60*1000LL)))continue;
            if(op=='P' && paused==value)continue;
            int64_t position=op=='S'?value*info.rate/1000:audible(pcm,submitted);
            snd_pcm_drop(pcm);
            position=audio_seek(decoder,position);
            if(position<0) { snprintf(error,sizeof(error),"%s",audio_error(decoder)); failed=quit=1; break; }
            submitted=anchor=position; count=offset=eof=0; began=progress=now();
            if(op=='P')paused=(int)value;
            if(snd_pcm_prepare(pcm)<0) { strcpy(error,"Cannot restart audio device"); failed=quit=1; break; }
            printf("POS %lld\nSTATE %s\n",(long long)(position*1000/info.rate),paused?"paused":"playing");
        }
        if(quit)break;
        uint64_t t=now();
        if(t>=next) { printf("POS %lld\n",(long long)(audible(pcm,submitted)*1000/info.rate)); next=t+250; }
        /* Limit producer lead even for ALSA null/file sinks; no busy decode loop. */
        int ahead=(submitted-anchor)*1000/info.rate>(int64_t)(t-began)+80;
        if(!paused && !ahead) {
            if(offset==count && !eof) {
                count=audio_read(decoder,samples,1024); offset=0;
                if(count<0) { snprintf(error,sizeof(error),"%s",audio_error(decoder)); failed=1; break; }
                if(!count)eof=1;
                for(int i=0;i<count*(int)info.channels;i++)samples[i]=(int16_t)((int)samples[i]*volume/100);
            }
            if(offset<count) {
                snd_pcm_sframes_t n=snd_pcm_writei(pcm,samples+(size_t)offset*info.channels,(snd_pcm_uframes_t)(count-offset));
                if(n>0) { offset+=(int)n; submitted+=n; progress=t; }
                else if(n<0 && n!=-EAGAIN && n!=-EINTR) {
                    if(n==-EPIPE && snd_pcm_prepare(pcm)>=0) {
                        /* Drop queued frames on an underrun; preserve source order. */
                        puts("XRUN");
                        began=t; anchor=submitted;
                    } else { snprintf(error,sizeof(error),"Playback: %s",snd_strerror((int)n)); failed=1; break; }
                }
            }
            if(eof && (submitted-anchor)*1000/info.rate<=(int64_t)(t-began)) {
                rc=snd_pcm_drain(pcm);
                if(rc==0) { printf("POS %lld\nSTATE ended\n",(long long)(submitted*1000/info.rate)); break; }
                if(rc!=-EAGAIN && rc!=-EINTR) { snprintf(error,sizeof(error),"Audio drain: %s",snd_strerror(rc)); failed=1; break; }
            }
            if(t-progress>10000) { strcpy(error,"Audio device stopped accepting samples"); failed=1; break; }
        }
        if(paused)progress=t;
        struct pollfd p={.fd=STDIN_FILENO,.events=POLLIN}; poll(&p,1,paused?100:10);
    }
    if(failed)printf("ERROR %s\n",error[0]?error:"Invalid player command");
    snd_pcm_drop(pcm); snd_pcm_close(pcm); audio_close(decoder); return failed?1:0;
}
