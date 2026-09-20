// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "archive_io.h"
#include "x11_controls.h"
#include "x11_preferences.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

static struct ui u;
static struct ui_text path,parent;
static struct archive_result *result;
static pid_t worker;
static int width=570,height=380,dirty=1,focus,top[2],scroll,extracting;
static char output[PATH_MAX],message[192]="Enter a ZIP or tar archive; Enter lists contents";
static volatile sig_atomic_t stopped,cancelled;
static uint64_t started,refresh,paste_until;
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void stop(int n) { (void)n; stopped=1; }
static void cancel_signal(int n) { (void)n; cancelled=1; }
static unsigned long color(const char *s)
{ XColor c,e; return XAllocNamedColor(u.display,DefaultColormap(u.display,DefaultScreen(u.display)),s,&c,&e)?c.pixel:0; }
static int rows(void) { int n=(height-202)/18; return n>0?n:1; }
static void cancel(void)
{
    if(!worker)return;
    /* Private partial output is retained even if a stuck codec needs SIGKILL. */
    kill(worker,SIGKILL); while(waitpid(worker,NULL,0)<0 && errno==EINTR) {}
    worker=0; snprintf(message,sizeof(message),"Cancelled%s",extracting?"; INCOMPLETE folder retained":""); dirty=1;
}
static void start(int extract)
{
    if(worker)return;
    if(!path.length || (extract && !parent.length)) { strcpy(message,"Enter archive and destination parent paths"); dirty=1; return; }
    int destination=-1; output[0]=0;
    if(extract) {
        char resolved[PATH_MAX];
        if(!realpath(parent.data,resolved))goto directory_error;
        if(snprintf(output,sizeof(output),"%s/wiidesk-extract-XXXXXX",resolved)>=(int)sizeof(output)) { errno=ENAMETOOLONG; goto directory_error; }
        if(!mkdtemp(output))goto directory_error;
        destination=open(output,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(destination<0)goto directory_error;
        int marker=openat(destination,ARCHIVE_PARTIAL,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(marker<0) { close(destination); goto directory_error; }
        const char warning[]="Extraction is incomplete. This marker is removed only on success.\n";
        ssize_t written=write(marker,warning,sizeof(warning)-1); int failed=close(marker);
        if(written!=(ssize_t)sizeof(warning)-1 || failed) { close(destination); errno=EIO; goto directory_error; }
    }
    ui_clipboard_cancel(&u);
    memset(result,0,sizeof(*result)); scroll=0; extracting=extract;
    worker=fork();
    if(!worker) {
        close(ConnectionNumber(u.display)); signal(SIGTERM,cancel_signal); signal(SIGINT,cancel_signal);
        prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()==1)_exit(1);
        struct rlimit memory={48*1024*1024,48*1024*1024},cpu={15,15},core={0,0};
        if(setrlimit(RLIMIT_AS,&memory) || setrlimit(RLIMIT_CPU,&cpu) || setrlimit(RLIMIT_CORE,&core))_exit(2);
        archive_process(path.data,destination,result,&cancelled);
        if(destination>=0 && result->ok && unlinkat(destination,ARCHIVE_PARTIAL,0)) {
            result->ok=0; strcpy(result->error,"Cannot remove incomplete marker");
        }
        if(destination>=0)close(destination);
        _exit(result->ok?0:1);
    }
    if(destination>=0)close(destination);
    if(worker<0) { worker=0; strcpy(message,"Cannot start worker; any output folder is INCOMPLETE"); }
    else { started=now(); snprintf(message,sizeof(message),"%s... Escape cancels",extract?"Extracting":"Inspecting"); }
    dirty=1; return;
directory_error:
    snprintf(message,sizeof(message),"Destination: %s",strerror(errno)); dirty=1;
}
static void tick(void)
{
    if(!worker)return;
    int status; pid_t done=waitpid(worker,&status,WNOHANG);
    if(done==worker) {
        worker=0;
        if(WIFEXITED(status) && !WEXITSTATUS(status) && result->ok)
            snprintf(message,sizeof(message),"%s: %u entries, %u bytes",extracting?"Extracted":"Checked",atomic_load(&result->count),atomic_load(&result->bytes));
        else snprintf(message,sizeof(message),"%.140s%s",result->error[0]?result->error:"Worker stopped (memory/time limit)",extracting?"; INCOMPLETE folder":"");
        dirty=1;
    } else if(now()-started>120000) { cancel(); strcpy(message,extracting?"Timed out; INCOMPLETE folder retained":"Inspection timed out"); }
    else if(now()>=refresh) { dirty=1; refresh=now()+250; }
}
static void field(struct ui_text *t,int y,int index)
{ ui_fill(&u,82,y,width-92,24,focus==index?u.surface:u.background); ui_text_draw(&u,t,86,y+4,width-100,16,&top[index],0); }
static void draw(void)
{
    u.width=width; u.height=height; XClearWindow(u.display,u.window);
    ui_label(&u,10,27,"Archive",u.muted); field(&path,10,0);
    ui_label(&u,10,59,"Into",u.muted); field(&parent,42,1);
    ui_button(&u,10,76,72,"List",0); ui_button(&u,88,76,144,"Extract new folder",0);
    if(worker)ui_button(&u,238,76,84,"Cancel",0);
    unsigned count=atomic_load(&result->count),bytes=atomic_load(&result->bytes);
    unsigned input=atomic_load(&result->input),size=atomic_load(&result->input_size);
    if(input>size)input=size;
    ui_fill(&u,10,108,width-20,6,u.surface);
    if(size)ui_fill(&u,10,108,(int)((uint64_t)input*(width-20)/size),6,u.accent);
    char progress[120]; snprintf(progress,sizeof(progress),"Read %u/%u bytes | %u entries | %u bytes decoded",input,size,count,bytes);
    ui_label(&u,12,134,progress,u.muted);
    if(scroll>(int)count-rows())scroll=(int)count-rows();
    if(scroll<0)scroll=0;
    for(int i=0;i<rows() && i+scroll<(int)count;i++) {
        const struct archive_row *row=&result->rows[i+scroll]; char line[600];
        snprintf(line,sizeof(line),"%c %9u  %s",row->directory?'D':'F',row->size,row->name);
        ui_label(&u,12,156+i*18,line,u.text);
    }
    const char *base=strrchr(output,'/');
    char location[160]; snprintf(location,sizeof(location),"Folder: %.150s",output[0]?(base?base+1:output):"(created on extraction)");
    if(output[0])ui_button(&u,width-106,height-52,96,"Copy path",0);
    ui_label(&u,12,height-34,location,u.muted); ui_label(&u,12,height-12,message,u.muted);
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_ARCHIVE_STATUS",False),XA_STRING,8,PropModeReplace,(unsigned char *)message,(int)strlen(message));
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_ARCHIVE_OUTPUT",False),XA_STRING,8,PropModeReplace,(unsigned char *)output,(int)strlen(output));
    XFlush(u.display); dirty=0;
}
static void copy_output(Time time)
{
    if(!output[0])return;
    struct ui_text text;
    if(ui_text_init(&text,PATH_MAX-1))return;
    ui_text_set(&text,output,strlen(output)); text.anchor=0; text.cursor=text.length;
    ui_clipboard_key(&u,&text,XK_c,ControlMask,time); ui_text_free(&text);
}
static void key(XKeyEvent *e)
{
    char text[32]; KeySym k; int n=XLookupString(e,text,sizeof(text),&k,NULL);
    if(k==XK_Escape) { cancel(); return; }
    if((e->state&(ControlMask|ShiftMask))==(ControlMask|ShiftMask) && (k==XK_c || k==XK_C)) { copy_output(e->time); return; }
    if(k==XK_Page_Up || k==XK_Page_Down) { scroll+=k==XK_Page_Up?-rows():rows(); dirty=1; return; }
    if(worker)return;
    if(k==XK_Return) { start((e->state&ControlMask)!=0); return; }
    if(k==XK_Tab) { focus=!focus; dirty=1; return; }
    struct ui_text *t=focus?&parent:&path;
    if(ui_clipboard_key(&u,t,k,e->state,e->time)) { paste_until=u.paste_target?now()+3000:0; dirty=1; return; }
    ui_text_key(t,k,e->state,text,n,0); dirty=1;
}
int main(int argc,char **argv)
{
    if(argc>2) { fprintf(stderr,"Usage: %s [ARCHIVE]\n",argv[0]); return 1; }
    u.display=XOpenDisplay(NULL); if(!u.display)return 1;
    result=mmap(NULL,sizeof(*result),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(result==MAP_FAILED)return 1;
    struct preferences prefs; preferences_load(&prefs);
    u.text=color("#eef2f3"); u.muted=color("#aabdc3"); u.surface=color("#333b40"); u.background=color("#242b2f"); u.accent=color(accent_colors[prefs.accent]);
    u.window=XCreateSimpleWindow(u.display,DefaultRootWindow(u.display),22,24,width,height,0,0,u.background);
    XStoreName(u.display,u.window,"WiiDesk Archives");
    unsigned long pid=(unsigned long)getpid(); XChangeProperty(u.display,u.window,XInternAtom(u.display,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,(unsigned char *)&pid,1);
    XClassHint cl={.res_name="archives",.res_class="WiiDesk"}; XSetClassHint(u.display,u.window,&cl);
    XSizeHints hints={.flags=PMinSize,.min_width=420,.min_height=280}; XSetWMNormalHints(u.display,u.window,&hints);
    Atom close_atom=XInternAtom(u.display,"WM_DELETE_WINDOW",False),protocols=XInternAtom(u.display,"WM_PROTOCOLS",False); XSetWMProtocols(u.display,u.window,&close_atom,1);
    XSelectInput(u.display,u.window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask);
    u.gc=XCreateGC(u.display,u.window,0,NULL); XFontStruct *font=XLoadQueryFont(u.display,"8x13"); if(!font)font=XLoadQueryFont(u.display,"fixed"); if(font)XSetFont(u.display,u.gc,font->fid);
    ui_clipboard_init(&u); if(ui_text_init(&path,PATH_MAX-1) || ui_text_init(&parent,PATH_MAX-1))return 1;
    const char *home=getenv("HOME"); if(home)ui_text_set(&parent,home,strlen(home));
    signal(SIGTERM,stop); signal(SIGINT,stop); XMapWindow(u.display,u.window);
    if(argc==2) { ui_text_set(&path,argv[1],strlen(argv[1])); start(0); }
    while(!stopped) {
        while(XPending(u.display) && !stopped) {
            XEvent e; XNextEvent(u.display,&e);
            if(ui_clipboard_event(&u,&e)) { dirty=1; continue; }
            if(e.type==Expose)dirty=1;
            else if(e.type==ConfigureNotify) { width=e.xconfigure.width; height=e.xconfigure.height; dirty=1; }
            else if(e.type==ClientMessage && e.xclient.message_type==protocols && (Atom)e.xclient.data.l[0]==close_atom)stopped=1;
            else if(e.type==KeyPress)key(&e.xkey);
            else if(e.type==ButtonPress) {
                int x=e.xbutton.x,y=e.xbutton.y;
                if(e.xbutton.button==4 || e.xbutton.button==5)scroll+=e.xbutton.button==4?-3:3;
                else if(e.xbutton.button==1) {
                    if(output[0] && ui_hit(x,y,width-106,height-52,96,24))copy_output(e.xbutton.time);
                    else if(worker) { if(ui_hit(x,y,238,76,84,24))cancel(); }
                    else if(ui_hit(x,y,10,76,72,24))start(0);
                    else if(ui_hit(x,y,88,76,144,24))start(1);
                    else if(y>=10 && y<66) { focus=y>=42; ui_text_click(focus?&parent:&path,x-86,0,width-100,top[focus],0,e.xbutton.state&ShiftMask); }
                }
                dirty=1;
            }
        }
        tick(); if(stopped)break;
        if(u.paste_target && now()>paste_until)ui_clipboard_cancel(&u);
        if(dirty)draw();
        struct pollfd fd={.fd=ConnectionNumber(u.display),.events=POLLIN}; poll(&fd,1,worker?100:u.paste_target?100:-1);
    }
    cancel(); munmap(result,sizeof(*result)); ui_text_free(&path); ui_text_free(&parent); ui_clipboard_free(&u);
    if(font)XFreeFont(u.display,font);
    XFreeGC(u.display,u.gc); XDestroyWindow(u.display,u.window); XCloseDisplay(u.display); return 0;
}
