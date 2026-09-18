// SPDX-License-Identifier: GPL-2.0-only
/* Observe client throughput alongside gcn_vi scanout tracing; XSync is not vblank. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int compare(const void *a,const void *b) { double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y); }
int main(int argc,char **argv)
{
    if(argc!=2 || (strcmp(argv[1],"draw") && strcmp(argv[1],"upload")))return 2;
    Display *d=XOpenDisplay(NULL); if(!d)return 1;
    int s=DefaultScreen(d),w=560,h=340;
    Window win=XCreateSimpleWindow(d,RootWindow(d,s),30,35,w,h,0,0,0);
    XStoreName(d,win,"WiiDesk frame-rate measurement (10 seconds)");
    XClassHint cl={.res_name="frame-probe",.res_class="WiiDeskTest"}; XSetClassHint(d,win,&cl);
    GC gc=XCreateGC(d,win,0,NULL); XMapRaised(d,win); XSync(d,False); sleep(2);
    XImage *im=NULL;
    if(!strcmp(argv[1],"upload")) {
        im=XCreateImage(d,DefaultVisual(d,s),DefaultDepth(d,s),ZPixmap,0,NULL,w,h,32,0);
        if(!im)return 1;
        im->data=calloc((size_t)im->bytes_per_line,h); if(!im->data)return 1;
        /* Initialize once: tests upload throughput, not per-pixel conversion. */
        for(int y=0;y<h;y++)for(int x=0;x<w;x++)XPutPixel(im,x,y,(unsigned long)((x*31/w)<<11)|((y*63/h)<<5));
    }
    double times[1000],begin=now(),finish=begin+10,next=begin; int frames=0;
    while(now()<finish && frames<1000) {
        double a=now();
        if(im)XPutImage(d,win,gc,im,0,0,0,0,w,h);
        else { XSetForeground(d,gc,0x2104); XFillRectangle(d,win,gc,0,0,w,h); }
        XSetForeground(d,gc,WhitePixel(d,s)); XFillRectangle(d,win,gc,(frames*7)%(w-40),80,40,160);
        char label[80]; snprintf(label,sizeof(label),"%s frame %d - target 60 updates/sec",argv[1],frames);
        XDrawString(d,win,gc,12,24,label,(int)strlen(label));
        XSync(d,False); times[frames++]=(now()-a)*1000;
        next+=1.0/60;
        double delay=next-now();
        if(delay>0) { struct timespec t={.tv_sec=0,.tv_nsec=(long)(delay*1e9)}; nanosleep(&t,NULL); }
        else next=now();
    }
    double end=now(); qsort(times,frames,sizeof(times[0]),compare);
    printf("mode=%s start=%.6f end=%.6f frames=%d client_fps=%.2f sync_ms_p50=%.2f sync_ms_p95=%.2f sync_ms_max=%.2f\n",
        argv[1],begin,end,frames,frames/(end-begin),times[frames/2],times[(frames-1)*95/100],times[frames-1]);
    if(im)XDestroyImage(im);
    XFreeGC(d,gc); XDestroyWindow(d,win); XCloseDisplay(d); return 0;
}
