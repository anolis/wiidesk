// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "package_io.h"
#include "x11_controls.h"
#include "x11_preferences.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>

static struct ui u;
static struct ui_text path;
static struct package_result *result;
static char directory[PATH_MAX],message[256]="Choose one .deb or a folder containing a bundle";
static char sibling[PATH_MAX];
static pid_t worker,terminal;
static int width=570,height=400,dirty=1,top,selected,scroll,ready,have_log,install_done;
static volatile sig_atomic_t stopped;
static uint64_t started,next,paste_until;
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void stop(int n) { (void)n; stopped=1; }
static unsigned long color(const char *s)
{ XColor c,e; return XAllocNamedColor(u.display,DefaultColormap(u.display,DefaultScreen(u.display)),s,&c,&e)?c.pixel:0; }
static int program(char *out,const char *name)
{ return snprintf(out,PATH_MAX,"%s/%s",sibling,name)>=PATH_MAX?-1:0; }
static void drop_snapshots(void)
{
    if(!directory[0])return;
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if(dir<0)return;
    for(unsigned i=0;i<PACKAGE_MAX;i++) { char file[32]; snprintf(file,sizeof(file),"%02u.deb",i); unlinkat(dir,file,0); }
    close(dir); ready=0;
}
static void dispose(void)
{
    if(!directory[0])return;
    drop_snapshots();
    if(!have_log) {
        int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(dir>=0) {
            for(unsigned i=0;i<PACKAGE_MAX;i++) { char file[32]; snprintf(file,sizeof(file),"metadata-%02u.txt",i); unlinkat(dir,file,0); }
            close(dir); rmdir(directory);
        }
    }
    directory[0]=0;
}
static void cancel(void)
{
    if(!worker)return;
    kill(worker,SIGTERM); kill(worker,SIGCONT); while(waitpid(worker,NULL,0)<0 && errno==EINTR) {}
    worker=0; ready=0; strcpy(message,"Inspection cancelled"); drop_snapshots(); dirty=1;
}
static void inspect(void)
{
    if(worker || terminal)return;
    dispose(); have_log=0; install_done=0;
    snprintf(directory,sizeof(directory),"/var/tmp/wiidesk-packages-XXXXXX");
    if(!mkdtemp(directory)) { directory[0]=0; strcpy(message,"Cannot create package staging folder"); dirty=1; return; }
    memset(result,0,sizeof(*result)); strcpy(result->spool,directory); selected=scroll=0;
    ui_clipboard_cancel(&u);
    worker=fork();
    if(!worker) {
        close(ConnectionNumber(u.display)); signal(SIGTERM,package_cancel); signal(SIGINT,package_cancel);
        prctl(PR_SET_PDEATHSIG,SIGTERM); if(getppid()==1)_exit(1);
        package_inspect(path.data,result); _exit(result->ok?0:1);
    }
    if(worker<0) { worker=0; strcpy(message,"Cannot start inspection"); }
    else { started=now(); strcpy(message,"Inspecting snapshots... Escape cancels"); }
    dirty=1;
}
static void view_file(int log)
{
    if(!directory[0] || (log && !have_log) || (!log && !atomic_load(&result->count)))return;
    char app[PATH_MAX],file[PATH_MAX]; if(program(app,"wiidesk-x11-app"))return;
    int n=log?snprintf(file,sizeof(file),"%s/install.log",directory):snprintf(file,sizeof(file),"%s/metadata-%02d.txt",directory,selected);
    if(n>=(int)sizeof(file))return;
    if(!fork()) { close(ConnectionNumber(u.display)); execl(app,app,"logs",file,(char *)NULL); _exit(127); }
}
static void install(void)
{
    if(worker || terminal || !ready)return;
    char runner[PATH_MAX]; if(program(runner,"wiidesk-package-terminal"))return;
    unsigned count=atomic_load(&result->count);
    terminal=fork();
    if(!terminal) {
        close(ConnectionNumber(u.display)); setsid();
        char *args[PACKAGE_MAX*2+10]={"/usr/bin/xterm","-title","WiiDesk Package Installation","-geometry","78x22","-e",runner,directory};
        for(unsigned i=0;i<count;i++) { args[8+i*2]=result->entries[i].digest; args[9+i*2]=result->entries[i].file; }
        args[8+count*2]=NULL; execv(args[0],args); _exit(127);
    }
    if(terminal<0) { terminal=0; strcpy(message,"Cannot open installation terminal"); }
    else { ready=0; have_log=1; install_done=0; strcpy(message,"Authenticate in terminal. Keep it open until dpkg finishes."); }
    dirty=1;
}
static void tick(void)
{
    int status;
    if(worker && waitpid(worker,&status,WNOHANG)==worker) {
        worker=0;
        ready=WIFEXITED(status) && !WEXITSTATUS(status) && result->ok;
        if(ready)snprintf(message,sizeof(message),"Review %u package(s); Install opens sudo authentication",atomic_load(&result->count));
        else snprintf(message,sizeof(message),"%s",result->error[0]?result->error:"Inspection failed");
        dirty=1;
    }
    if(worker && now()-started>120000) { cancel(); strcpy(message,"Inspection timed out"); }
    if(terminal) {
        int exited=waitpid(terminal,&status,WNOHANG)==terminal;
        if(!install_done) {
            char file[PATH_MAX];
            if(snprintf(file,sizeof(file),"%s/result",directory)<(int)sizeof(file)) {
                FILE *f=fopen(file,"r"); int rc;
                if(f) { if(fscanf(f,"%d",&rc)==1) {
                    install_done=1;
                    snprintf(message,sizeof(message),rc?"Install failed (code %d); see Log. Packages may be unconfigured.":"Installed and verified successfully",rc);
                    drop_snapshots(); dirty=1;
                } fclose(f); }
            }
        }
        if(exited) {
            terminal=0;
            if(!install_done) { strcpy(message,"Terminal ended without a result; inspect Log and package state."); dirty=1; }
        }
    }
    if(!worker && !terminal)while(waitpid(-1,NULL,WNOHANG)>0) {}
    if((worker || terminal) && now()>=next) { dirty=1; next=now()+250; }
}
static int rows(void) { return 4; }
static void draw(void)
{
    u.width=width; u.height=height; XClearWindow(u.display,u.window);
    ui_fill(&u,10,10,width-20,26,u.surface); ui_text_draw(&u,&path,14,15,width-28,16,&top,0);
    ui_button(&u,10,44,84,"Inspect",0); ui_button(&u,100,44,84,"Details",0);
    ui_button(&u,190,44,96,"Install...",ready); ui_button(&u,292,44,60,"Log",0);
    if(worker)ui_button(&u,358,44,84,"Cancel",0);
    ui_label(&u,12,94,"Only install trusted packages; scripts run as administrator.",u.muted);
    unsigned count=atomic_load(&result->count);
    if(selected>=(int)count)selected=count?(int)count-1:0;
    if(selected<scroll)scroll=selected;
    if(selected>=scroll+rows())scroll=selected-rows()+1;
    for(int i=0;i<rows() && i+scroll<(int)count;i++) {
        struct package_entry *e=&result->entries[i+scroll]; char line[340];
        snprintf(line,sizeof(line),"%s  %s  [%s]",e->name,e->version,e->arch);
        if(i+scroll==selected)ui_fill(&u,10,108+i*22,width-20,22,u.surface);
        ui_label(&u,14,124+i*22,line,u.text);
    }
    if(count) {
        struct package_entry *e=&result->entries[selected]; char line[512];
        snprintf(line,sizeof(line),"Installed: %s",e->installed); ui_label(&u,12,218,line,u.muted);
        snprintf(line,sizeof(line),"File: %u KiB | Declared installed size: %u KiB",e->size/1024,e->installed_kib); ui_label(&u,12,240,line,u.muted);
        ui_label(&u,12,264,e->description,u.text);
    }
    ui_label(&u,12,height-60,"Resolve/download dependencies on the host. No APT on Wii.",u.muted);
    if(worker) { char text[100]; snprintf(text,sizeof(text),"Inspected %u package(s)...",count); ui_label(&u,12,height-38,text,u.muted); }
    ui_label(&u,12,height-12,message,u.muted);
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_PACKAGE_STATUS",False),XA_STRING,8,PropModeReplace,(unsigned char *)message,(int)strlen(message));
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_PACKAGE_JOB",False),XA_STRING,8,PropModeReplace,(unsigned char *)directory,(int)strlen(directory));
    XFlush(u.display); dirty=0;
}
static void key(XKeyEvent *e)
{
    KeySym k; char text[32]; int n=XLookupString(e,text,sizeof(text),&k,NULL);
    if(k==XK_Escape) { cancel(); return; }
    if(k==XK_Up || k==XK_Down) { selected+=k==XK_Up?-1:1; if(selected<0)selected=0; dirty=1; return; }
    if(worker || terminal)return;
    if(k==XK_Return) { if(e->state&ControlMask)install(); else inspect(); return; }
    if(k==XK_F2) { view_file(0); return; }
    if(k==XK_F3) { view_file(1); return; }
    if(ui_clipboard_key(&u,&path,k,e->state,e->time)) {
        if(u.paste_target || ((e->state&ControlMask) && (k==XK_x || k==XK_X))) {
            if(ready)strcpy(message,"Path edited; inspect again before installing");
            ready=0;
        }
        paste_until=u.paste_target?now()+3000:0; dirty=1; return;
    }
    if(n>0 || k==XK_BackSpace || k==XK_Delete) {
        if(ready)strcpy(message,"Path edited; inspect again before installing");
        ready=0;
    }
    ui_text_key(&path,k,e->state,text,n,0); dirty=1;
}
int main(int argc,char **argv)
{
    if(argc>2 || geteuid()==0) { fprintf(stderr,"Run as the desktop user: %s [DEB_OR_FOLDER]\n",argv[0]); return 1; }
    ssize_t n=readlink("/proc/self/exe",sibling,sizeof(sibling)-1); if(n<0 || n>=(ssize_t)sizeof(sibling)-1)return 1;
    sibling[n]=0; char *base=strrchr(sibling,'/'); if(!base)return 1; *base=0;
    u.display=XOpenDisplay(NULL); if(!u.display)return 1;
    result=mmap(NULL,sizeof(*result),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0); if(result==MAP_FAILED)return 1;
    struct preferences prefs; preferences_load(&prefs);
    u.text=color("#eef2f3"); u.muted=color("#aabdc3"); u.surface=color("#333b40"); u.background=color("#242b2f"); u.accent=color(accent_colors[prefs.accent]);
    u.window=XCreateSimpleWindow(u.display,DefaultRootWindow(u.display),22,20,width,height,0,0,u.background); XStoreName(u.display,u.window,"WiiDesk Packages");
    unsigned long pid=getpid(); XChangeProperty(u.display,u.window,XInternAtom(u.display,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,(unsigned char *)&pid,1);
    XClassHint cl={.res_name="packages",.res_class="WiiDesk"}; XSetClassHint(u.display,u.window,&cl);
    XSizeHints hints={.flags=PMinSize,.min_width=500,.min_height=370}; XSetWMNormalHints(u.display,u.window,&hints);
    Atom close_atom=XInternAtom(u.display,"WM_DELETE_WINDOW",False),protocols=XInternAtom(u.display,"WM_PROTOCOLS",False); XSetWMProtocols(u.display,u.window,&close_atom,1);
    XSelectInput(u.display,u.window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask);
    u.gc=XCreateGC(u.display,u.window,0,NULL); XFontStruct *font=XLoadQueryFont(u.display,"8x13"); if(!font)font=XLoadQueryFont(u.display,"fixed"); if(font)XSetFont(u.display,u.gc,font->fid);
    ui_clipboard_init(&u); if(ui_text_init(&path,PATH_MAX-1))return 1;
    signal(SIGTERM,stop); signal(SIGINT,stop); XMapWindow(u.display,u.window);
    if(argc==2) { ui_text_set(&path,argv[1],strlen(argv[1])); inspect(); }
    while(!stopped) {
        while(XPending(u.display) && !stopped) {
            XEvent e; XNextEvent(u.display,&e); if(ui_clipboard_event(&u,&e)) { dirty=1; continue; }
            if(e.type==Expose)dirty=1;
            else if(e.type==ConfigureNotify) { width=e.xconfigure.width; height=e.xconfigure.height; dirty=1; }
            else if(e.type==ClientMessage && e.xclient.message_type==protocols && (Atom)e.xclient.data.l[0]==close_atom) {
                if(terminal && !install_done) { strcpy(message,"Finish installation in the terminal before closing this window."); dirty=1; }
                else stopped=1;
            } else if(e.type==KeyPress)key(&e.xkey);
            else if(e.type==ButtonPress && e.xbutton.button==1) {
                int x=e.xbutton.x,y=e.xbutton.y;
                if(ui_hit(x,y,358,44,84,24) && worker)cancel();
                else if(ui_hit(x,y,100,44,84,24))view_file(0);
                else if(ui_hit(x,y,292,44,60,24))view_file(1);
                else if(!worker && !terminal) {
                    if(ui_hit(x,y,10,44,84,24))inspect();
                    else if(ui_hit(x,y,190,44,96,24))install();
                    else if(ui_hit(x,y,10,10,width-20,26))ui_text_click(&path,x-14,0,width-28,top,0,e.xbutton.state&ShiftMask);
                }
                if(y>=108 && y<108+rows()*22)selected=scroll+(y-108)/22;
                dirty=1;
            }
        }
        tick(); if(stopped)break;
        if(u.paste_target && now()>paste_until)ui_clipboard_cancel(&u);
        if(dirty)draw();
        struct pollfd fd={.fd=ConnectionNumber(u.display),.events=POLLIN}; poll(&fd,1,worker || terminal?100:u.paste_target?100:-1);
    }
    cancel(); if(!terminal || install_done)dispose();
    ui_text_free(&path); ui_clipboard_free(&u); munmap(result,sizeof(*result));
    if(font)XFreeFont(u.display,font);
    XFreeGC(u.display,u.gc); XDestroyWindow(u.display,u.window); XCloseDisplay(u.display); return 0;
}
