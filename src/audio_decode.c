// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "audio_decode.h"
#include <mpg123.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct audio_decoder {
    int fd,bits,done;
    struct audio_info info;
    int64_t start,position;
    mpg123_handle *mp3;
    char error[160];
};
static unsigned le16(const unsigned char *p) { return p[0]|(unsigned)p[1]<<8; }
static uint32_t le32(const unsigned char *p) { return le16(p)|(uint32_t)le16(p+2)<<16; }
static int read_at(int fd,void *data,size_t size,off_t offset)
{
    size_t used=0;
    while(used<size) {
        ssize_t n=pread(fd,(char *)data+used,size-used,offset+(off_t)used);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;
        used+=(size_t)n;
    }
    return 0;
}
static int wav(struct audio_decoder *d,off_t size,const unsigned char *header)
{
    uint64_t end=(uint64_t)le32(header+4)+8,at=12,data_size=0;
    int format=0,data=0;
    if(end>(uint64_t)size || end<12)goto invalid;
    for(unsigned chunks=0;at+8<=end && chunks<1024;chunks++) {
        unsigned char h[24]; if(read_at(d->fd,h,8,(off_t)at))goto invalid;
        uint32_t n=le32(h+4); at+=8;
        if(n>end-at)goto invalid;
        if(!memcmp(h,"fmt ",4)) {
            if(format || n<16 || read_at(d->fd,h+8,16,(off_t)at))goto invalid;
            unsigned channels=le16(h+10),rate=le32(h+12),bits=le16(h+22);
            if(le16(h+8)!=1 || channels<1 || channels>2 || rate<8000 || rate>48000 ||
               (bits!=8 && bits!=16) || le16(h+20)!=channels*(bits/8) || le32(h+16)!=rate*channels*(bits/8))goto invalid;
            d->info.channels=channels; d->info.rate=rate; d->bits=(int)bits; format=1;
        } else if(!memcmp(h,"data",4)) {
            if(data)goto invalid;
            d->start=(int64_t)at; data_size=n; data=1;
        }
        at+=(uint64_t)n+(n&1);
    }
    if(at!=end || !format || !data || !data_size || data_size%(d->info.channels*(d->bits/8)))goto invalid;
    d->info.frames=(int64_t)(data_size/(d->info.channels*(d->bits/8))); return 0;
invalid:
    strcpy(d->error,"Invalid WAV: use PCM 8/16-bit, mono/stereo, 8-48 kHz"); return -1;
}
struct audio_decoder *audio_open(const char *path,struct audio_info *info,char *error,size_t capacity)
{
    struct audio_decoder *d=calloc(1,sizeof(*d)); if(!d) { snprintf(error,capacity,"Out of memory"); return NULL; }
    d->fd=open(path,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    struct stat st; unsigned char h[12];
    if(d->fd<0 || fstat(d->fd,&st) || !S_ISREG(st.st_mode) || st.st_size<12 || st.st_size>1024LL*1024*1024 || read_at(d->fd,h,sizeof(h),0)) {
        strcpy(d->error,"Open failed: regular audio file required (up to 1 GiB)"); goto fail;
    }
    if(!memcmp(h,"RIFF",4) && !memcmp(h+8,"WAVE",4)) {
        if(wav(d,st.st_size,h))goto fail;
    } else {
        int code,encoding,channels; long rate;
        if(mpg123_init()!=MPG123_OK || !(d->mp3=mpg123_new(NULL,&code))) { strcpy(d->error,"Cannot initialize MP3 decoder"); goto fail; }
        mpg123_param(d->mp3,MPG123_ADD_FLAGS,MPG123_QUIET|MPG123_SKIP_ID3V2|MPG123_NO_RESYNC,0);
        mpg123_param(d->mp3,MPG123_INDEX_SIZE,1000,0);
        mpg123_format_none(d->mp3);
        const long rates[]={8000,11025,12000,16000,22050,24000,32000,44100,48000};
        for(unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);i++)mpg123_format(d->mp3,rates[i],MPG123_MONO|MPG123_STEREO,MPG123_ENC_SIGNED_16);
        if(mpg123_open_fd(d->mp3,d->fd)!=MPG123_OK || mpg123_getformat(d->mp3,&rate,&channels,&encoding)!=MPG123_OK || encoding!=MPG123_ENC_SIGNED_16 || rate<8000 || rate>48000 || channels<1 || channels>2) {
            strcpy(d->error,"Invalid or unsupported MP3 audio"); goto fail;
        }
        mpg123_format_none(d->mp3); mpg123_format(d->mp3,rate,channels,encoding);
        d->info.rate=(unsigned)rate; d->info.channels=(unsigned)channels;
        d->info.frames=mpg123_length(d->mp3); d->info.estimated=1;
    }
    *info=d->info; return d;
fail:
    snprintf(error,capacity,"%s",d->error); audio_close(d); return NULL;
}
int audio_read(struct audio_decoder *d,int16_t *out,unsigned frames)
{
    if(frames>4096)frames=4096;
    if(d->mp3) {
        if(d->done)return 0;
        size_t bytes=0; int rc=mpg123_read(d->mp3,out,(size_t)frames*d->info.channels*2,&bytes);
        if(rc!=MPG123_OK && rc!=MPG123_DONE) { strcpy(d->error,"MP3 decoding failed or format changed"); return -1; }
        if(bytes%(d->info.channels*2)) { strcpy(d->error,"Incomplete MP3 sample"); return -1; }
        if(rc==MPG123_DONE)d->done=1;
        int n=(int)(bytes/(d->info.channels*2)); d->position+=n; return n;
    }
    if(d->position>=d->info.frames)return 0;
    if((int64_t)frames>d->info.frames-d->position)frames=(unsigned)(d->info.frames-d->position);
    unsigned samples=frames*d->info.channels,bytes=samples*(d->bits/8);
    unsigned char buffer[16384];
    if(read_at(d->fd,buffer,bytes,d->start+d->position*d->info.channels*(d->bits/8))) { strcpy(d->error,"WAV data truncated or unreadable"); return -1; }
    for(unsigned i=0;i<samples;i++)out[i]=d->bits==8?(int16_t)(((int)buffer[i]-128)*256):(int16_t)le16(buffer+i*2);
    d->position+=frames; return (int)frames;
}
int64_t audio_seek(struct audio_decoder *d,int64_t frame)
{
    if(frame<0)frame=0;
    if(d->info.frames>=0 && frame>d->info.frames)frame=d->info.frames;
    int64_t at=d->mp3?(int64_t)mpg123_seek(d->mp3,(off_t)frame,SEEK_SET):frame;
    if(at<0) { strcpy(d->error,"Cannot seek in this audio file"); return -1; }
    d->position=at; d->done=0; return at;
}
const char *audio_error(struct audio_decoder *d) { return d->error; }
void audio_close(struct audio_decoder *d)
{ if(!d)return; if(d->mp3)mpg123_delete(d->mp3); if(d->fd>=0)close(d->fd); free(d); }
