// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "x11_utilities.h"
#include "x11_controls.h"
#include "x11_preferences.h"
#include "calculator.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINES 512
#define LOG_WIDTH 256
static struct ui u;
static struct ui_text input, filter;
static int calculator, width=500, height=350, visible=1, dirty=1;
static int focus, input_top, filter_top, paused, scroll, count, matches;
static int match[LOG_LINES];
static char lines[LOG_LINES][LOG_WIDTH];
static char path[PATH_MAX], status[192];
static char history[64][257], results[64][64];
static int histories, selected_history=-1;
static struct stat stamp;
static int have_stamp;
static volatile sig_atomic_t stopped;
static uint64_t paste_until;
static void stop(int signal_number) { (void)signal_number; stopped=1; }
static uint64_t milliseconds(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static unsigned long color(const char *s)
{
    XColor c, exact;
    return XAllocNamedColor(u.display,DefaultColormap(u.display,DefaultScreen(u.display)),s,&c,&exact) ? c.pixel : 0;
}
static int rows(void) { int r=(height-150)/20; return r>0?r:1; }
static void rebuild_matches(void)
{
    matches=0;
    for(int i=0;i<count;i++) if(!filter.length || strstr(lines[i],filter.data)) match[matches++]=i;
    int max=matches>rows()?matches-rows():0;
    if(scroll>max) scroll=max;
    if(scroll<0) scroll=0;
    if(!calculator && have_stamp) snprintf(status,sizeof(status),"%d matching / %d lines | Last 64 KiB, up to 512 lines",matches,count);
}
static void log_refresh(int force)
{
    if(!path[0] || (!force && paused)) return;
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    if(fd<0) { snprintf(status,sizeof(status),"Open failed: %s",strerror(errno)); return; }
    if(fstat(fd,&st) || !S_ISREG(st.st_mode)) { close(fd); strcpy(status,"Choose a regular log file"); return; }
    if(!force && have_stamp && st.st_dev==stamp.st_dev && st.st_ino==stamp.st_ino && st.st_size==stamp.st_size &&
       st.st_mtim.tv_sec==stamp.st_mtim.tv_sec && st.st_mtim.tv_nsec==stamp.st_mtim.tv_nsec) { close(fd); return; }
    off_t start=st.st_size>65536?st.st_size-65536:0;
    if(lseek(fd,start,SEEK_SET)<0) { close(fd); strcpy(status,"Could not seek log"); return; }
    char data[65537]; size_t used=0;
    while(used<sizeof(data)-1) {
        ssize_t n=read(fd,data+used,sizeof(data)-1-used);
        if(n<0 && errno==EINTR) continue;
        if(n<0) { close(fd); strcpy(status,"Log read failed"); return; }
        if(!n) break;
        used+=(size_t)n;
    }
    close(fd); data[used]=0; count=0;
    size_t begin=0;
    if(start) { while(begin<used && data[begin]!='\n') begin++; if(begin<used) begin++; }
    /* Skip excess lines before copying. Repeatedly shifting a full 128 KiB
     * table makes a tail of many short lines disproportionately expensive. */
    size_t remaining=used>begin && data[used-1]!='\n'?1:0;
    for(size_t i=begin;i<used;i++)if(data[i]=='\n')remaining++;
    while(remaining>LOG_LINES) {
        while(begin<used && data[begin]!='\n')begin++;
        if(begin<used)begin++;
        remaining--;
    }
    for(size_t i=begin;i<used;) {
        size_t end=i; while(end<used && data[end]!='\n') end++;
        size_t n=end-i; if(n>=LOG_WIDTH) n=LOG_WIDTH-1;
        for(size_t j=0;j<n;j++) { unsigned char c=data[i+j]; lines[count][j]=c>=32 && c<127?(char)c:' '; }
        lines[count][n]=0;
        if(end-i>=LOG_WIDTH) memcpy(lines[count]+LOG_WIDTH-4,"...",4);
        count++; i=end+1;
    }
    stamp=st; have_stamp=1; rebuild_matches();
    if(!paused) scroll=matches>rows()?matches-rows():0;
    snprintf(status,sizeof(status),"%d matching / %d lines | Last 64 KiB, up to 512 lines",matches,count);
    dirty=1;
}
static void open_log(void)
{
    snprintf(path,sizeof(path),"%s",input.data); have_stamp=0; log_refresh(1); dirty=1;
}
static void evaluate(void)
{
    double result;
    if(calculator_eval(input.data,&result,status,sizeof(status))) { dirty=1; return; }
    if(histories==64) { memmove(history,history+1,63*sizeof(history[0])); memmove(results,results+1,63*sizeof(results[0])); histories--; }
    snprintf(history[histories],sizeof(history[0]),"%s",input.data);
    snprintf(results[histories],sizeof(results[0]),"%.12g",result);
    snprintf(status,sizeof(status),"= %.12g",result); histories++; selected_history=-1; scroll=0; dirty=1;
}
static void draw(void)
{
    u.width=width; u.height=height; XClearWindow(u.display,u.window);
    ui_fill(&u,10,10,width-20,26,focus==0?u.surface:u.background);
    ui_text_draw(&u,&input,14,15,width-28,16,&input_top,0);
    if(calculator) {
        ui_button(&u,10,44,100,"Evaluate",0); ui_button(&u,120,44,120,"Clear history",0);
        ui_label(&u,12,88,"+ - * / % ^ | sin cos tan sqrt log ln exp abs",u.muted);
        ui_label(&u,12,108,"Radians | pi, e | Up/Down recalls expressions",u.muted);
        for(int row=0;row<rows() && row+scroll<histories;row++) {
            int i=histories-1-row-scroll; char line[340];
            snprintf(line,sizeof(line),"%s = %s",history[i],results[i]);
            ui_label(&u,12,132+row*20,line,u.text);
        }
    } else {
        ui_button(&u,10,44,90,"Open",0); ui_button(&u,110,44,100,paused?"Follow":"Pause",0);
        ui_label(&u,12,94,"Filter:",u.muted);
        ui_fill(&u,76,76,width-86,26,focus==1?u.surface:u.background);
        ui_text_draw(&u,&filter,80,81,width-94,16,&filter_top,0);
        for(int row=0;row<rows() && scroll+row<matches;row++) ui_label(&u,12,128+row*20,lines[match[scroll+row]],u.text);
    }
    ui_label(&u,12,height-12,status,u.muted);
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_UTILITY_STATUS",False),XA_STRING,8,PropModeReplace,(unsigned char *)status,(int)strlen(status));
    XFlush(u.display); dirty=0;
}
static void recall(int direction)
{
    if(!histories) return;
    selected_history+=direction;
    if(selected_history<0) selected_history=0;
    if(selected_history>=histories) selected_history=histories-1;
    ui_text_set(&input,history[histories-1-selected_history],strlen(history[histories-1-selected_history]));
}
static void key(XKeyEvent *e)
{
    KeySym k; char bytes[32]; int n=XLookupString(e,bytes,sizeof(bytes),&k,NULL);
    if(k==XK_Escape) { stopped=1; return; }
    if(k==XK_Tab && !calculator) { focus=!focus; dirty=1; return; }
    if(k==XK_Return) { if(calculator) evaluate(); else if(!focus) open_log(); return; }
    if(calculator && (k==XK_Up || k==XK_Down)) { recall(k==XK_Up?1:-1); dirty=1; return; }
    if(!calculator && (k==XK_Page_Up || k==XK_Page_Down)) { paused=1; scroll+=k==XK_Page_Up?-rows():rows(); rebuild_matches(); dirty=1; return; }
    if(!calculator && k==XK_F5) { log_refresh(1); return; }
    if(!calculator && k==XK_F6) { paused=!paused; if(!paused)log_refresh(1); dirty=1; return; }
    struct ui_text *field=focus?&filter:&input;
    int result=ui_clipboard_key(&u,field,k,e->state,e->time);
    if(!result) result=ui_text_key(field,k,e->state,bytes,n,0);
    if(result<0) strcpy(status,"Input rejected: ASCII text or size limit");
    if(u.paste_target) paste_until=milliseconds()+3000;
    if(!calculator && focus) { scroll=0; rebuild_matches(); }
    dirty=1;
}
static void button(XButtonEvent *e)
{
    if(e->button==Button4 || e->button==Button5) {
        scroll+=e->button==Button4?-1:1;
        if(calculator) { if(scroll<0)scroll=0; if(scroll>=histories)scroll=histories?histories-1:0; }
        else { paused=1; rebuild_matches(); }
    }
    if(e->button==Button1) {
        if(ui_hit(e->x,e->y,10,10,width-20,26)) { focus=0; ui_text_click(&input,e->x-14,0,width-28,input_top,0,e->state&ShiftMask); }
        else if(ui_hit(e->x,e->y,10,44,100,24)) { if(calculator)evaluate(); else open_log(); }
        else if(ui_hit(e->x,e->y,120,44,120,24) && calculator) { histories=0; selected_history=-1; scroll=0; }
        else if(ui_hit(e->x,e->y,110,44,100,24) && !calculator) { paused=!paused; if(!paused)log_refresh(1); }
        else if(!calculator && ui_hit(e->x,e->y,76,76,width-86,26)) { focus=1; ui_text_click(&filter,e->x-80,0,width-94,filter_top,0,e->state&ShiftMask); }
    }
    dirty=1;
}
int x11_utility_supported(const char *name) { return !strcmp(name,"calculator") || !strcmp(name,"logs"); }
int x11_utility_main(int argc,char **argv)
{
    calculator=!strcmp(argv[1],"calculator");
    if(argc>3 || (calculator && argc!=2)) { fprintf(stderr,"Usage: %s calculator | logs [FILE]\n",argv[0]); return 1; }
    u.display=XOpenDisplay(NULL); if(!u.display) return 1;
    struct preferences prefs; preferences_load(&prefs);
    u.text=color("#eef2f3"); u.muted=color("#aabdc3"); u.background=color("#242b2f");
    u.surface=color("#333b40"); u.accent=color(accent_colors[prefs.accent]);
    u.window=XCreateSimpleWindow(u.display,DefaultRootWindow(u.display),36,36,width,height,0,0,u.background);
    XStoreName(u.display,u.window,calculator?"WiiDesk Calculator":"WiiDesk Logs");
    XClassHint class_hint={.res_name=argv[1],.res_class="WiiDesk"}; XSetClassHint(u.display,u.window,&class_hint);
    XSizeHints hints={.flags=PMinSize,.min_width=360,.min_height=260}; XSetWMNormalHints(u.display,u.window,&hints);
    Atom close_atom=XInternAtom(u.display,"WM_DELETE_WINDOW",False), protocols=XInternAtom(u.display,"WM_PROTOCOLS",False);
    XSetWMProtocols(u.display,u.window,&close_atom,1);
    XSelectInput(u.display,u.window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask|VisibilityChangeMask);
    u.gc=XCreateGC(u.display,u.window,0,NULL); XFontStruct *font=XLoadQueryFont(u.display,"8x13");
    if(!font)font=XLoadQueryFont(u.display,"fixed");
    if(font)XSetFont(u.display,u.gc,font->fid);
    ui_clipboard_init(&u);
    if(ui_text_init(&input,calculator?256:PATH_MAX-1) || ui_text_init(&filter,128)) return 1;
    if(!calculator) {
        const char *initial=argc==3?argv[2]:"/var/log/wiidesk-Xorg.1.log";
        ui_text_set(&input,initial,strlen(initial)); open_log();
    } else strcpy(status,"Enter an expression; Enter evaluates");
    signal(SIGTERM,stop); signal(SIGINT,stop); XMapWindow(u.display,u.window);
    uint64_t next=milliseconds()+1000;
    while(!stopped) {
        while(XPending(u.display) && !stopped) {
            XEvent e; XNextEvent(u.display,&e);
            int clip=ui_clipboard_event(&u,&e);
            if(clip) { if(clip<0)strcpy(status,"Paste rejected"); rebuild_matches(); dirty=1; continue; }
            switch(e.type) {
            case Expose: dirty=1; break;
            case ConfigureNotify: width=e.xconfigure.width; height=e.xconfigure.height; rebuild_matches(); dirty=1; break;
            case MapNotify: visible=1; dirty=1; break;
            case UnmapNotify: visible=0; break;
            case VisibilityNotify: visible=e.xvisibility.state!=VisibilityFullyObscured; break;
            case KeyPress: key(&e.xkey); break;
            case ButtonPress: button(&e.xbutton); break;
            case ClientMessage: if(e.xclient.message_type==protocols && (Atom)e.xclient.data.l[0]==close_atom)stopped=1; break;
            case DestroyNotify: stopped=1; break;
            default: break;
            }
        }
        uint64_t now=milliseconds();
        if(!calculator && visible && !paused && now>=next) { log_refresh(0); next=now+1000; }
        if(u.paste_target && now>=paste_until) { ui_clipboard_cancel(&u); strcpy(status,"Paste timed out"); dirty=1; }
        if(dirty && visible) draw();
        if(stopped)break;
        int timeout=!calculator && visible && !paused?(int)(next>now?next-now:0):-1;
        if(u.paste_target && (timeout<0 || timeout>100))timeout=100;
        struct pollfd fd={.fd=ConnectionNumber(u.display),.events=POLLIN};
        if(poll(&fd,1,timeout)<0 && errno!=EINTR)break;
        if(fd.revents&(POLLERR|POLLHUP|POLLNVAL))break;
    }
    ui_clipboard_free(&u); ui_text_free(&input); ui_text_free(&filter);
    if(font)XFreeFont(u.display,font);
    XFreeGC(u.display,u.gc); XCloseDisplay(u.display); return 0;
}
