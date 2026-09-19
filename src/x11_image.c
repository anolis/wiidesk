// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "image_decode.h"
#include "x11_controls.h"
#include "x11_preferences.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>

static struct ui u;
static struct ui_text path;
static struct image_result *result;
static XImage *view;
static pid_t worker;
static int width=540,height=380,dirty=1,rebuild=1,top,fit=1,zoom=100,pan_x,pan_y;
static int have_image;
static char message[192]="Open a PNG or JPEG; Enter opens the path", current[PATH_MAX];
static volatile sig_atomic_t stopped;
static uint64_t started,paste_until;
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void stop(int sig) { (void)sig; stopped=1; }
static unsigned long color(const char *s)
{ XColor c,e; return XAllocNamedColor(u.display,DefaultColormap(u.display,DefaultScreen(u.display)),s,&c,&e)?c.pixel:0; }
static void cancel(void)
{
    if(!worker)return;
    kill(worker,SIGKILL);
    while(waitpid(worker,NULL,0)<0 && errno==EINTR) {}
    worker=0; strcpy(message,"Image loading cancelled"); dirty=1;
}
static void load(int direction)
{
    char requested[PATH_MAX]; snprintf(requested,sizeof(requested),"%s",direction?current:path.data);
    if(!requested[0]) { strcpy(message,"Enter an image path first"); dirty=1; return; }
    cancel();
    if(view) { XDestroyImage(view); view=NULL; }
    have_image=0; result->ok=0; result->error[0]=0; rebuild=1;
    worker=fork();
    if(!worker) {
        close(ConnectionNumber(u.display));
        prctl(PR_SET_PDEATHSIG,SIGKILL);
        if(getppid()==1)_exit(1);
        struct rlimit memory={32*1024*1024,32*1024*1024}, cpu={5,5}, core={0,0};
        if(setrlimit(RLIMIT_AS,&memory) || setrlimit(RLIMIT_CPU,&cpu) || setrlimit(RLIMIT_CORE,&core))_exit(2);
        image_decode(requested,direction,result); _exit(result->ok?0:1);
    }
    if(worker<0) { worker=0; strcpy(message,"Cannot start image decoder"); }
    else { started=now(); strcpy(message,"Loading image... Escape cancels"); }
    dirty=1;
}
static void check_worker(void)
{
    if(!worker)return;
    int status; pid_t done=waitpid(worker,&status,WNOHANG);
    if(done==worker) {
        worker=0;
        if(WIFEXITED(status) && !WEXITSTATUS(status) && result->ok) {
            have_image=1; fit=1; pan_x=pan_y=0;
            snprintf(current,sizeof(current),"%s",result->path);
            ui_text_set(&path,current,strlen(current));
            snprintf(message,sizeof(message),"%u x %u | Fit | PNG/JPEG",result->width,result->height);
        } else snprintf(message,sizeof(message),"%s",result->error[0]?result->error:"Decoder stopped: memory/time limit or invalid image");
        rebuild=dirty=1;
    } else if(now()-started>15000) { cancel(); strcpy(message,"Image loading timed out (15 seconds)"); }
}
static unsigned long channel(unsigned value,unsigned long mask)
{
    if(!mask)return 0;
    unsigned shift=0; while(!(mask&1)) { mask>>=1; shift++; }
    return ((value*mask+127)/255)<<shift;
}
static void make_view(void)
{
    if(view) { XDestroyImage(view); view=NULL; }
    if(!have_image)return;
    int vw=width-20,vh=height-116;
    if(vw<1 || vh<1)return;
    if(vw>1024)vw=1024;
    if(vh>768)vh=768;
    double scale=fit?fmin((double)vw/result->width,(double)vh/result->height):zoom/100.0;
    if(scale>4)scale=4;
    int sw=(int)(result->width*scale),sh=(int)(result->height*scale);
    if(sw<1)sw=1;
    if(sh<1)sh=1;
    int w=sw<vw?sw:vw,h=sh<vh?sh:vh;
    if(pan_x>sw-w)pan_x=sw-w;
    if(pan_y>sh-h)pan_y=sh-h;
    if(pan_x<0)pan_x=0;
    if(pan_y<0)pan_y=0;
    view=XCreateImage(u.display,DefaultVisual(u.display,DefaultScreen(u.display)),
                     (unsigned)DefaultDepth(u.display,DefaultScreen(u.display)),ZPixmap,0,NULL,(unsigned)w,(unsigned)h,32,0);
    if(!view || (size_t)view->bytes_per_line*h>4*1024*1024)goto fail;
    view->data=calloc((size_t)view->bytes_per_line,(size_t)h);
    if(!view->data)goto fail;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        unsigned sx=(unsigned)((x+pan_x)/scale),sy=(unsigned)((y+pan_y)/scale);
        if(sx>=result->width)sx=result->width-1;
        if(sy>=result->height)sy=result->height-1;
        const unsigned char *p=result->rgb+((size_t)sy*result->width+sx)*3;
        XPutPixel(view,x,y,channel(p[0],view->red_mask)|channel(p[1],view->green_mask)|channel(p[2],view->blue_mask));
    }
    snprintf(message,sizeof(message),"%u x %u | %s %d%% | Alt+arrows pan; PgUp/PgDn browse",result->width,result->height,fit?"Fit":"Zoom",(int)(scale*100));
    return;
fail:
    if(view) { XDestroyImage(view); view=NULL; }
    strcpy(message,"Not enough display memory");
}
static void draw(void)
{
    if(rebuild) { make_view(); rebuild=0; }
    u.width=width; u.height=height; XClearWindow(u.display,u.window);
    ui_fill(&u,10,10,width-20,26,u.surface);
    ui_text_draw(&u,&path,14,15,width-28,16,&top,0);
    ui_button(&u,10,44,60,"Open",0); ui_button(&u,76,44,64,"Prev",0);
    ui_button(&u,146,44,64,"Next",0); ui_button(&u,216,44,54,"Fit",fit);
    ui_button(&u,276,44,54,"1:1",!fit && zoom==100);
    ui_button(&u,336,44,36,"-",0); ui_button(&u,378,44,36,"+",0);
    if(worker)ui_button(&u,420,44,72,"Cancel",0);
    if(view)XPutImage(u.display,u.window,u.gc,view,0,0,10+(width-20-view->width)/2,80+(height-116-view->height)/2,(unsigned)view->width,(unsigned)view->height);
    else ui_label(&u,12,106,worker?"Decoding...":"PNG / JPEG, up to 1 megapixel and 16 MiB",u.muted);
    ui_label(&u,12,height-12,message,u.muted);
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_IMAGE_STATUS",False),XA_STRING,8,PropModeReplace,(unsigned char *)message,(int)strlen(message));
    XFlush(u.display); dirty=0;
}
static void adjust(int change)
{ fit=0; zoom+=change; if(zoom<25)zoom=25; if(zoom>400)zoom=400; rebuild=dirty=1; }
static void key(XKeyEvent *e)
{
    char text[32]; KeySym k; int n=XLookupString(e,text,sizeof(text),&k,NULL);
    if(k==XK_Escape) { cancel(); return; }
    if(k==XK_Return) { load(0); return; }
    if(k==XK_Page_Down || k==XK_Page_Up) { load(k==XK_Page_Down?1:-1); return; }
    if(k==XK_F5) { fit=1; pan_x=pan_y=0; rebuild=dirty=1; return; }
    if(e->state&Mod1Mask) {
        if(k==XK_Left)pan_x-=32;
        if(k==XK_Right)pan_x+=32;
        if(k==XK_Up)pan_y-=32;
        if(k==XK_Down)pan_y+=32;
        rebuild=dirty=1; return;
    }
    if(ui_clipboard_key(&u,&path,k,e->state,e->time)) { paste_until=u.paste_target?now()+3000:0; dirty=1; return; }
    ui_text_key(&path,k,e->state,text,n,0); dirty=1;
}
int main(int argc,char **argv)
{
    if(argc>2) { fprintf(stderr,"Usage: %s [PNG_OR_JPEG]\n",argv[0]); return 1; }
    u.display=XOpenDisplay(NULL); if(!u.display)return 1;
    if(DefaultVisual(u.display,DefaultScreen(u.display))->class!=TrueColor) { fprintf(stderr,"Image Viewer requires a TrueColor display\n"); return 1; }
    result=mmap(NULL,sizeof(*result),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(result==MAP_FAILED)return 1;
    struct preferences prefs; preferences_load(&prefs);
    u.text=color("#eef2f3"); u.muted=color("#aabdc3"); u.surface=color("#333b40");
    u.background=color("#242b2f"); u.accent=color(accent_colors[prefs.accent]);
    u.window=XCreateSimpleWindow(u.display,DefaultRootWindow(u.display),30,30,width,height,0,0,u.background);
    XStoreName(u.display,u.window,"WiiDesk Images");
    unsigned long pid=(unsigned long)getpid();
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,(unsigned char *)&pid,1);
    XClassHint cl={.res_name="images",.res_class="WiiDesk"}; XSetClassHint(u.display,u.window,&cl);
    XSizeHints hints={.flags=PMinSize|PMaxSize,.min_width=500,.min_height=240,.max_width=1024,.max_height=768}; XSetWMNormalHints(u.display,u.window,&hints);
    Atom close_atom=XInternAtom(u.display,"WM_DELETE_WINDOW",False),protocols=XInternAtom(u.display,"WM_PROTOCOLS",False);
    XSetWMProtocols(u.display,u.window,&close_atom,1);
    XSelectInput(u.display,u.window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask);
    u.gc=XCreateGC(u.display,u.window,0,NULL); XFontStruct *font=XLoadQueryFont(u.display,"8x13");
    if(!font)font=XLoadQueryFont(u.display,"fixed");
    if(font)XSetFont(u.display,u.gc,font->fid);
    ui_clipboard_init(&u); if(ui_text_init(&path,PATH_MAX-1))return 1;
    signal(SIGTERM,stop); signal(SIGINT,stop);
    XMapWindow(u.display,u.window);
    if(argc==2) { ui_text_set(&path,argv[1],strlen(argv[1])); load(0); }
    while(!stopped) {
        while(XPending(u.display) && !stopped) {
            XEvent e; XNextEvent(u.display,&e);
            if(ui_clipboard_event(&u,&e)) { dirty=1; continue; }
            if(e.type==Expose)dirty=1;
            else if(e.type==ConfigureNotify) {
                if(width!=e.xconfigure.width || height!=e.xconfigure.height)rebuild=dirty=1;
                width=e.xconfigure.width; height=e.xconfigure.height;
            } else if(e.type==ClientMessage && e.xclient.message_type==protocols && (Atom)e.xclient.data.l[0]==close_atom)stopped=1;
            else if(e.type==KeyPress)key(&e.xkey);
            else if(e.type==ButtonPress) {
                int x=e.xbutton.x,y=e.xbutton.y;
                if(e.xbutton.button==4 || e.xbutton.button==5)adjust(e.xbutton.button==4?25:-25);
                else if(e.xbutton.button==1) {
                    if(ui_hit(x,y,10,10,width-20,26))ui_text_click(&path,x-14,0,width-28,top,0,e.xbutton.state&ShiftMask);
                    else if(ui_hit(x,y,10,44,60,24))load(0);
                    else if(ui_hit(x,y,76,44,64,24))load(-1);
                    else if(ui_hit(x,y,146,44,64,24))load(1);
                    else if(ui_hit(x,y,216,44,54,24)) { fit=1; pan_x=pan_y=0; rebuild=1; }
                    else if(ui_hit(x,y,276,44,54,24)) { fit=0; zoom=100; pan_x=pan_y=0; rebuild=1; }
                    else if(ui_hit(x,y,336,44,36,24))adjust(-25);
                    else if(ui_hit(x,y,378,44,36,24))adjust(25);
                    else if(ui_hit(x,y,420,44,72,24))cancel();
                }
                dirty=1;
            }
        }
        check_worker();
        if(u.paste_target && now()>paste_until)ui_clipboard_cancel(&u);
        if(stopped)break;
        if(dirty)draw();
        struct pollfd fd={.fd=ConnectionNumber(u.display),.events=POLLIN};
        poll(&fd,1,worker?50:u.paste_target?100:-1);
    }
    cancel(); if(view)XDestroyImage(view);
    munmap(result,sizeof(*result)); ui_text_free(&path); ui_clipboard_free(&u);
    if(font)XFreeFont(u.display,font);
    XFreeGC(u.display,u.gc); XDestroyWindow(u.display,u.window); XCloseDisplay(u.display); return 0;
}
