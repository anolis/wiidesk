// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "x11_network.h"
#include "x11_controls.h"
#include "x11_preferences.h"
#include "wifi_control.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct report { char lines[12][128]; char message[192]; int count; };
static struct report report, incoming;
static struct ui u;
static struct ui_text host, ssid, password;
static int width=500,height=380,visible=1,dirty=1,configure,focus,top[3];
static int job_fd=-1,job_kind,closing;
static size_t received;
static pid_t child;
static double job_start,paste_until;
static volatile sig_atomic_t stopped,cancelled;
static char message[192];
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void stop(int n) { (void)n; stopped=1; }
static void cancel(int n) { (void)n; cancelled=1; }
static unsigned long color(const char *s)
{ XColor c,e; return XAllocNamedColor(u.display,DefaultColormap(u.display,DefaultScreen(u.display)),s,&c,&e)?c.pixel:0; }
static void add(struct report *r,const char *a,const char *b)
{
    if(r->count>=12)return;
    snprintf(r->lines[r->count],sizeof(r->lines[0]),"%.24s %.96s",a,b);
    for(char *p=r->lines[r->count];*p;p++)if((unsigned char)*p<32 || (unsigned char)*p>126)*p=' ';
    r->count++;
}
static void snapshot(struct report *r)
{
    struct ifaddrs *interfaces=NULL;
    if(!getifaddrs(&interfaces)) {
        for(struct ifaddrs *p=interfaces;p && r->count<7;p=p->ifa_next) {
            if(!p->ifa_addr || p->ifa_addr->sa_family!=AF_INET || (p->ifa_flags&IFF_LOOPBACK))continue;
            char ip[INET_ADDRSTRLEN],mask[INET_ADDRSTRLEN],line[96];
            inet_ntop(AF_INET,&((struct sockaddr_in *)p->ifa_addr)->sin_addr,ip,sizeof(ip));
            strcpy(mask,"unknown");
            if(p->ifa_netmask)inet_ntop(AF_INET,&((struct sockaddr_in *)p->ifa_netmask)->sin_addr,mask,sizeof(mask));
            snprintf(line,sizeof(line),"%s  mask %s",ip,mask); add(r,p->ifa_name,line);
        }
        freeifaddrs(interfaces);
    }
    if(!r->count)add(r,"IPv4:","No non-loopback address");
    FILE *f=fopen("/proc/net/route","r"); char line[512];
    if(f) {
        while(fgets(line,sizeof(line),f)) {
            char name[32]; unsigned dest,gateway,flags;
            if(sscanf(line,"%31s %x %x %x",name,&dest,&gateway,&flags)==4 && !dest && (flags&2)) {
                struct in_addr a={.s_addr=gateway}; char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET,&a,ip,sizeof(ip)); add(r,"Gateway:",ip); break;
            }
        }
        fclose(f);
    }
    f=fopen("/etc/resolv.conf","r");
    if(f) {
        int dns=0;
        while(dns<2 && fgets(line,sizeof(line),f)) {
            char address[128]; if(sscanf(line,"nameserver %127s",address)==1) { add(r,"DNS:",address); dns++; }
        }
        fclose(f);
    }
    char state[4096];
    if(!wifi_status("/run/wpa_supplicant/wlan0",state,sizeof(state))) {
        char *save=NULL;
        for(char *p=strtok_r(state,"\n",&save);p;p=strtok_r(NULL,"\n",&save)) {
            if(!strncmp(p,"ssid=",5))add(r,"Wi-Fi:",p+5);
            if(!strncmp(p,"wpa_state=",10))add(r,"State:",p+10);
        }
    } else add(r,"Wi-Fi control:",errno==EACCES?"netdev membership needed":"unavailable");
}
static void connectivity(const char *target,char *out,size_t size)
{
    char name[257],port[8]="443";
    snprintf(name,sizeof(name),"%s",target);
    char *colon=strrchr(name,':');
    if(colon) {
        char *end; long number=strtol(colon+1,&end,10);
        if(end==colon+1 || *end || number<1 || number>65535) { snprintf(out,size,"Invalid port (1-65535)"); return; }
        snprintf(port,sizeof(port),"%ld",number); *colon=0;
    }
    if(!*name) { snprintf(out,size,"Enter a host name or IPv4 address"); return; }
    struct addrinfo hints={.ai_socktype=SOCK_STREAM,.ai_family=AF_UNSPEC},*addresses=NULL;
    int result=getaddrinfo(name,port,&hints,&addresses);
    if(result) { snprintf(out,size,"DNS lookup failed: %s",gai_strerror(result)); return; }
    int connected=0,tries=0;
    for(struct addrinfo *p=addresses;p && tries<3 && !cancelled;p=p->ai_next,tries++) {
        int fd=socket(p->ai_family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd<0)continue;
        result=connect(fd,p->ai_addr,p->ai_addrlen);
        if(!result)connected=1;
        else if(errno==EINPROGRESS) {
            struct pollfd pollfd={.fd=fd,.events=POLLOUT};
            if(poll(&pollfd,1,3000)>0) {
                int error=0; socklen_t length=sizeof(error);
                if(!getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) && !error)connected=1;
            }
        }
        close(fd); if(connected)break;
    }
    freeaddrinfo(addresses);
    snprintf(out,size,connected?"DNS/TCP connection succeeded (no data sent)":cancelled?"Connectivity check cancelled":"DNS resolved; TCP connection failed or timed out");
}
static void start_job(int kind)
{
    if(child)return;
    int pipefd[2];
    if(pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)) { snprintf(message,sizeof(message),"Cannot start check: %s",strerror(errno)); return; }
    pid_t parent=getpid(); child=fork();
    if(child<0) { child=0; close(pipefd[0]); close(pipefd[1]); strcpy(message,"Cannot start worker"); return; }
    if(!child) {
        close(pipefd[0]); close(ConnectionNumber(u.display));
        signal(SIGTERM,cancel); signal(SIGINT,cancel);
        prctl(PR_SET_PDEATHSIG,SIGTERM); if(getppid()!=parent)cancelled=1;
        struct report result={0};
        if(kind==2) (void)wifi_connect("/run/wpa_supplicant/wlan0",ssid.data,password.data,&cancelled,result.message,sizeof(result.message));
        if(kind==3)connectivity(host.data,result.message,sizeof(result.message));
        if(!cancelled)snapshot(&result);
        /* The fixed report is smaller than PIPE_BUF; no secret is included. */
        ssize_t written=write(pipefd[1],&result,sizeof(result)); close(pipefd[1]); _exit(written==(ssize_t)sizeof(result)?0:1);
    }
    close(pipefd[1]); job_fd=pipefd[0]; received=0; memset(&incoming,0,sizeof(incoming));
    job_kind=kind; job_start=now();
    if(kind!=1 || !report.count)strcpy(message,kind==2?"Connecting... Cancel restores the previous profile":kind==3?"Checking DNS/TCP...":"Refreshing network status...");
    dirty=1;
    if(kind==2) {
        explicit_bzero(password.data,password.capacity+1);
        if(password.undo)explicit_bzero(password.undo,password.capacity+1);
        password.length=password.cursor=password.anchor=0; password.has_undo=0;
    }
}
static void collect(void)
{
    if(!child)return;
    ssize_t n=read(job_fd,(char *)&incoming+received,sizeof(incoming)-received);
    if(n>0)received+=(size_t)n;
    if(job_kind!=2 && now()-job_start>15) { kill(child,SIGTERM); strcpy(message,"Network check timed out"); }
    if(job_kind!=2 && now()-job_start>16)kill(child,SIGKILL);
    int state; pid_t result=waitpid(child,&state,WNOHANG);
    if(result!=child)return;
    /* A final read handles the child exiting just before the pipe was read. */
    if(received<sizeof(incoming)) {
        n=read(job_fd,(char *)&incoming+received,sizeof(incoming)-received);
        if(n>0)received+=(size_t)n;
    }
    if(received==sizeof(incoming)) {
        report=incoming;
        if(incoming.message[0])snprintf(message,sizeof(message),"%s",incoming.message);
        else if(job_kind==1 && !strcmp(message,"Refreshing network status..."))strcpy(message,"Status refreshed; Wi-Fi changes require netdev access");
    } else if(!closing && now()-job_start<=15)strcpy(message,"Network worker stopped without a result");
    close(job_fd); job_fd=-1; child=0; dirty=1;
    if(closing)stopped=1;
}
static void draw_field(struct ui_text *field,int y,int selected,int index,int secret)
{
    ui_fill(&u,12,y,width-24,28,selected?u.surface:u.background);
    if(secret) {
        char mask[64]; memset(mask,'*',field->length); mask[field->length]=0;
        struct ui_text hidden=*field; hidden.data=mask;
        ui_text_draw(&u,&hidden,16,y+6,width-32,16,&top[index],0);
    } else ui_text_draw(&u,field,16,y+6,width-32,16,&top[index],0);
}
static void draw(void)
{
    u.width=width; u.height=height; XClearWindow(u.display,u.window);
    if(configure) {
        ui_label(&u,12,24,"Wi-Fi network name (SSID)",u.muted); draw_field(&ssid,32,focus==0,1,0);
        ui_label(&u,12,86,"WPA/WPA2 personal password",u.muted); draw_field(&password,94,focus==1,2,1);
        ui_button(&u,12,138,144,"Connect & save",0); ui_button(&u,166,138,90,"Back",0);
        ui_label(&u,12,190,"Changes the Wii's active Wi-Fi connection.",u.text);
        ui_label(&u,12,212,"Failed attempts restore the previous profile.",u.muted);
        ui_label(&u,12,234,"IP addresses are assigned automatically (DHCP).",u.muted);
    } else {
        ui_button(&u,12,10,100,"Refresh",0); ui_button(&u,122,10,144,"Wi-Fi setup",0);
        for(int i=0;i<report.count && i<(height-150)/20;i++)ui_label(&u,12,64+i*20,report.lines[i],u.text);
        ui_label(&u,12,height-94,"Connectivity target (host:port)",u.muted);
        draw_field(&host,height-84,1,0,0);
        ui_button(&u,12,height-48,136,"Check connection",0);
    }
    if(child)ui_button(&u,width-102,10,90,"Cancel",0);
    ui_label(&u,12,height-12,message,u.muted);
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_UTILITY_STATUS",False),XA_STRING,8,PropModeReplace,(unsigned char *)message,(int)strlen(message));
    XFlush(u.display); dirty=0;
}
static void close_request(void) { if(child) { closing=1; kill(child,SIGTERM); strcpy(message,"Cancelling network operation..."); dirty=1; } else stopped=1; }
static void key(XKeyEvent *e)
{
    KeySym k; char bytes[32]; int n=XLookupString(e,bytes,sizeof(bytes),&k,NULL);
    if(k==XK_Escape) { if(child)kill(child,SIGTERM); else if(configure)configure=0; else close_request(); dirty=1; return; }
    if(child)return;
    if(configure && k==XK_Tab) { focus=!focus; dirty=1; return; }
    if(k==XK_F2) { configure=!configure; focus=0; dirty=1; return; }
    if(k==XK_Return) { start_job(configure?2:3); return; }
    if(k==XK_F5) { start_job(1); return; }
    struct ui_text *field=configure?(focus?&password:&ssid):&host;
    int result=0;
    if(field!=&password || ((e->state&ControlMask) && (k==XK_v || k==XK_V)))result=ui_clipboard_key(&u,field,k,e->state,e->time);
    if(!result)result=ui_text_key(field,k,e->state,bytes,n,0);
    if(result<0)strcpy(message,"Input rejected: ASCII text or size limit");
    if(u.paste_target)paste_until=now()+3;
    dirty=1;
}
static void button(XButtonEvent *e)
{
    if(e->button!=Button1)return;
    if(child) { if(ui_hit(e->x,e->y,width-102,10,90,24))kill(child,SIGTERM); return; }
    if(configure) {
        if(ui_hit(e->x,e->y,12,32,width-24,28)) { focus=0; ui_text_click(&ssid,e->x-16,0,width-32,top[1],0,e->state&ShiftMask); }
        else if(ui_hit(e->x,e->y,12,94,width-24,28)) { focus=1; ui_text_click(&password,e->x-16,0,width-32,top[2],0,e->state&ShiftMask); }
        else if(ui_hit(e->x,e->y,12,138,144,24))start_job(2);
        else if(ui_hit(e->x,e->y,166,138,90,24))configure=0;
    } else {
        if(ui_hit(e->x,e->y,12,10,100,24))start_job(1);
        else if(ui_hit(e->x,e->y,122,10,144,24)) { configure=1; focus=0; }
        else if(ui_hit(e->x,e->y,12,height-48,136,24))start_job(3);
        else if(ui_hit(e->x,e->y,12,height-84,width-24,28))ui_text_click(&host,e->x-16,0,width-32,top[0],0,e->state&ShiftMask);
    }
    dirty=1;
}
int x11_network_main(void)
{
    u.display=XOpenDisplay(NULL); if(!u.display)return 1;
    struct preferences prefs; preferences_load(&prefs);
    u.text=color("#eef2f3"); u.muted=color("#aabdc3"); u.background=color("#242b2f"); u.surface=color("#333b40"); u.accent=color(accent_colors[prefs.accent]);
    u.window=XCreateSimpleWindow(u.display,DefaultRootWindow(u.display),36,30,width,height,0,0,u.background);
    XStoreName(u.display,u.window,"WiiDesk Network"); XClassHint cl={.res_name="network",.res_class="WiiDesk"}; XSetClassHint(u.display,u.window,&cl);
    XSizeHints hints={.flags=PMinSize,.min_width=420,.min_height=340}; XSetWMNormalHints(u.display,u.window,&hints);
    Atom deletion=XInternAtom(u.display,"WM_DELETE_WINDOW",False),protocols=XInternAtom(u.display,"WM_PROTOCOLS",False); XSetWMProtocols(u.display,u.window,&deletion,1);
    XSelectInput(u.display,u.window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask|VisibilityChangeMask);
    u.gc=XCreateGC(u.display,u.window,0,NULL); XFontStruct *font=XLoadQueryFont(u.display,"8x13");
    if(!font)font=XLoadQueryFont(u.display,"fixed");
    if(font)XSetFont(u.display,u.gc,font->fid);
    ui_clipboard_init(&u);
    if(ui_text_init(&host,256) || ui_text_init(&ssid,32) || ui_text_init(&password,63))return 1;
    ui_text_set(&host,"example.com:443",15);
    signal(SIGTERM,stop); signal(SIGINT,stop); XMapWindow(u.display,u.window); start_job(1);
    double next=now()+5;
    while(!stopped) {
        while(XPending(u.display) && !stopped) {
            XEvent e; XNextEvent(u.display,&e);
            int clip=ui_clipboard_event(&u,&e); if(clip) { if(clip<0)strcpy(message,"Paste rejected"); dirty=1; continue; }
            switch(e.type) {
            case Expose: dirty=1; break;
            case ConfigureNotify: width=e.xconfigure.width; height=e.xconfigure.height; dirty=1; break;
            case VisibilityNotify: visible=e.xvisibility.state!=VisibilityFullyObscured; break;
            case MapNotify: visible=1; dirty=1; break;
            case UnmapNotify: visible=0; break;
            case KeyPress: key(&e.xkey); break;
            case ButtonPress: button(&e.xbutton); break;
            case ClientMessage: if(e.xclient.message_type==protocols && (Atom)e.xclient.data.l[0]==deletion)close_request(); break;
            case DestroyNotify: visible=0; close_request(); break;
            default: break;
            }
        }
        collect();
        if(visible && !configure && !child && now()>=next) { start_job(1); next=now()+5; }
        if(u.paste_target && now()>=paste_until) { ui_clipboard_cancel(&u); strcpy(message,"Paste timed out"); dirty=1; }
        if(dirty && visible)draw();
        if(stopped)break;
        struct pollfd fds[2]={{.fd=ConnectionNumber(u.display),.events=POLLIN},{.fd=job_fd,.events=POLLIN}};
        if(poll(fds,child?2:1,child || u.paste_target?100:visible && !configure?1000:-1)<0 && errno!=EINTR)break;
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))break;
    }
    if(child) { kill(child,SIGTERM); waitpid(child,NULL,0); }
    if(job_fd>=0)close(job_fd);
    explicit_bzero(password.data,password.capacity+1); if(password.undo)explicit_bzero(password.undo,password.capacity+1);
    ui_clipboard_free(&u); ui_text_free(&host); ui_text_free(&ssid); ui_text_free(&password);
    if(font)XFreeFont(u.display,font);
    XFreeGC(u.display,u.gc); XCloseDisplay(u.display); return 0;
}
