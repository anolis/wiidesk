// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "wifi_control.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
struct control { int fd; char directory[64], local[108]; };
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void finish(struct control *c)
{
    if(c->fd>=0)close(c->fd);
    if(c->local[0])unlink(c->local);
    if(c->directory[0])rmdir(c->directory);
}
static int connect_control(struct control *c,const char *server)
{
    memset(c,0,sizeof(*c)); c->fd=-1;
    snprintf(c->directory,sizeof(c->directory),"/tmp/wiidesk-wifi-XXXXXX");
    if(!mkdtemp(c->directory)) { c->directory[0]=0; return -1; }
    snprintf(c->local,sizeof(c->local),"%s/control",c->directory);
    struct sockaddr_un a={.sun_family=AF_UNIX};
    if(strlen(server)>=sizeof(a.sun_path)) { errno=ENAMETOOLONG; goto fail; }
    c->fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0); if(c->fd<0)goto fail;
    strcpy(a.sun_path,c->local);
    if(bind(c->fd,(struct sockaddr *)&a,sizeof(a)))goto fail;
    strcpy(a.sun_path,server);
    if(connect(c->fd,(struct sockaddr *)&a,sizeof(a)))goto fail;
    return 0;
fail:;
    int saved=errno; finish(c); errno=saved; return -1;
}
static int request(struct control *c,const char *command,char *reply,size_t size)
{
    if(size<2) { errno=EINVAL; return -1; }
    if(send(c->fd,command,strlen(command),0)<0)return -1;
    struct pollfd p={.fd=c->fd,.events=POLLIN};
    int rc=poll(&p,1,2000);
    if(rc<=0) { if(!rc)errno=ETIMEDOUT; return -1; }
    ssize_t n=recv(c->fd,reply,size-1,MSG_TRUNC);
    if(n<0)return -1;
    if((size_t)n>=size) { errno=EMSGSIZE; return -1; }
    reply[n]=0; return 0;
}
static int command_ok(struct control *c,const char *command)
{
    char reply[256];
    if(request(c,command,reply,sizeof(reply)))return -1;
    if(strcmp(reply,"OK\n") && strcmp(reply,"OK")) { errno=EACCES; return -1; }
    return 0;
}
int wifi_status(const char *path,char *reply,size_t size)
{
    struct control c;
    if(connect_control(&c,path))return -1;
    int result=request(&c,"STATUS",reply,size); int saved=errno;
    finish(&c); errno=saved; return result;
}
static int network_id(const char *status)
{
    const char *p=status;
    while(*p) {
        if(!strncmp(p,"id=",3)) {
            char *end; long id=strtol(p+3,&end,10);
            if(end!=p+3 && (*end=='\n' || !*end) && id>=0 && id<=1000000)return (int)id;
        }
        p=strchr(p,'\n'); if(!p)break; p++;
    }
    return -1;
}
int wifi_connect(const char *path,const char *ssid,const char *password,
                 volatile sig_atomic_t *cancelled,char *message,size_t size)
{
    size_t sn=strlen(ssid),pn=strlen(password);
    if(!sn || sn>32 || pn<8 || pn>63) {
        snprintf(message,size,"SSID: 1-32 bytes; password: 8-63 ASCII characters"); return -1;
    }
    for(size_t i=0;i<pn;i++) if((unsigned char)password[i]<32 || (unsigned char)password[i]>126) {
        snprintf(message,size,"Password must be printable ASCII"); return -1;
    }
    char hex[65];
    for(size_t i=0;i<sn;i++)snprintf(hex+2*i,3,"%02x",(unsigned char)ssid[i]);
    char quoted[129]; size_t q=0;
    for(size_t i=0;i<pn;i++) { if(password[i]=='"' || password[i]=='\\')quoted[q++]='\\'; quoted[q++]=password[i]; }
    quoted[q]=0;
    struct control c;
    if(connect_control(&c,path)) {
        snprintf(message,size,"Wi-Fi control unavailable: %s",strerror(errno)); explicit_bzero(quoted,sizeof(quoted)); return -1;
    }
    char reply[4096],command[256]; int previous=-1,id=-1,result=-1;
    if(request(&c,"STATUS",reply,sizeof(reply)))goto io_error;
    previous=network_id(reply);
    if(*cancelled)goto rollback;
    if(request(&c,"ADD_NETWORK",reply,sizeof(reply)))goto io_error;
    char *end; long parsed=strtol(reply,&end,10);
    if(end==reply || (*end && *end!='\n') || parsed<0 || parsed>1000000) { errno=EPROTO; goto io_error; }
    id=(int)parsed;
    snprintf(command,sizeof(command),"SET_NETWORK %d ssid %s",id,hex);
    if(command_ok(&c,command))goto io_error;
    snprintf(command,sizeof(command),"SET_NETWORK %d psk \"%s\"",id,quoted);
    if(command_ok(&c,command))goto io_error;
    explicit_bzero(command,sizeof(command)); explicit_bzero(quoted,sizeof(quoted));
    if(*cancelled)goto rollback;
    snprintf(command,sizeof(command),"SELECT_NETWORK %d",id);
    if(command_ok(&c,command))goto io_error;
    double deadline=now()+30;
    while(!*cancelled && now()<deadline) {
        if(request(&c,"STATUS",reply,sizeof(reply)))goto io_error;
        if(network_id(reply)==id && strstr(reply,"wpa_state=COMPLETED\n")) {
            if(*cancelled)goto rollback;
            if(command_ok(&c,"SAVE_CONFIG")) {
                snprintf(message,size,"Connected, but saving failed; restoring prior connection"); goto rollback;
            }
            snprintf(message,size,"Wi-Fi connected and saved; DHCP may take a few seconds"); result=0; goto done;
        }
        struct timespec pause={.tv_sec=0,.tv_nsec=200000000}; nanosleep(&pause,NULL);
    }
    snprintf(message,size,*cancelled?"Connection cancelled; restoring previous profile":"Connection timed out; restoring previous profile");
    goto rollback;
io_error:
    snprintf(message,size,"Wi-Fi request failed: %s",strerror(errno));
rollback:
    if(*cancelled)snprintf(message,size,"Connection cancelled; restoring previous profile");
    if(id>=0) {
        int failed=0;
        if(previous>=0) { snprintf(command,sizeof(command),"SELECT_NETWORK %d",previous); failed=command_ok(&c,command); }
        else failed=command_ok(&c,"DISCONNECT");
        snprintf(command,sizeof(command),"REMOVE_NETWORK %d",id); failed|=command_ok(&c,command);
        if(failed)snprintf(message,size,"Connection failed; automatic rollback failed too. Check Wi-Fi status.");
    }
done:
    explicit_bzero(command,sizeof(command)); explicit_bzero(quoted,sizeof(quoted));
    finish(&c); return result;
}
