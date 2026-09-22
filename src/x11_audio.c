// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "audio_playlist.h"
#include "x11_controls.h"
#include "x11_preferences.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static struct ui u;
static struct ui_text path;
static struct audio_playlist *list;
static struct ui_list selection;
static int width=580,height=400,dirty=1,top,focus_path=1,volume=50,repeat,active=-1;
static int visible=1,underruns;
static int control=-1,output=-1,paused,ended,failed,estimated;
static pid_t player,loader;
static char executable[PATH_MAX],message[240]="Open WAV, MP3, a folder, or a local M3U playlist";
static char incoming[512]; static size_t incoming_used;
static int64_t position,duration=-1;
static uint64_t heartbeat,load_started,paste_until;
static volatile sig_atomic_t stopped;
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void stop_signal(int n) { (void)n; stopped=1; }
static unsigned long color(const char *s)
{ XColor c,e; return XAllocNamedColor(u.display,DefaultColormap(u.display,DefaultScreen(u.display)),s,&c,&e)?c.pixel:0; }
static void kill_child(pid_t *pid)
{ if(*pid>0) { kill(*pid,SIGKILL); while(waitpid(*pid,NULL,0)<0 && errno==EINTR) {} *pid=0; } }
static void stop_player(void)
{
    kill_child(&player);
    if(control>=0)close(control);
    if(output>=0)close(output);
    control=output=-1; incoming_used=0; paused=ended=failed=estimated=underruns=0; active=-1; position=0; duration=-1;
}
static void command(char kind,int64_t value)
{
    if(control<0)return;
    char text[64]; int n=snprintf(text,sizeof(text),"%c %lld\n",kind,(long long)value);
    if(write(control,text,(size_t)n)!=n) { stop_player(); strcpy(message,"Player control failed; playback stopped"); }
    dirty=1;
}
static void load(void)
{
    stop_player(); kill_child(&loader); ui_clipboard_cancel(&u);
    memset(list,0,sizeof(*list)); selection=(struct ui_list){0};
    loader=fork();
    if(!loader) { close(ConnectionNumber(u.display)); prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()==1)_exit(1); audio_playlist_load(path.data,list); _exit(list->ok?0:1); }
    if(loader<0) { loader=0; strcpy(message,"Cannot load playlist"); }
    else { load_started=now(); strcpy(message,"Loading playlist... Escape cancels"); }
    dirty=1;
}
static void play(int index)
{
    if(loader || !list->ok || index<0 || index>=list->count)return;
    stop_player();
    int in[2],out[2];
    if(pipe2(in,O_CLOEXEC)) { strcpy(message,"Cannot start audio worker"); dirty=1; return; }
    if(pipe2(out,O_CLOEXEC)) { close(in[0]); close(in[1]); strcpy(message,"Cannot start audio worker"); dirty=1; return; }
    player=fork();
    if(!player) {
        close(ConnectionNumber(u.display)); dup2(in[0],STDIN_FILENO); dup2(out[1],STDOUT_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()==1)_exit(1);
        const char *device=getenv("WIIDESK_AUDIO_DEVICE"); if(!device || !device[0])device="default";
        execl(executable,executable,list->paths[index],device,(char *)NULL); _exit(127);
    }
    close(in[0]); close(out[1]);
    if(player<0) { player=0; close(in[1]); close(out[0]); strcpy(message,"Cannot start audio worker"); }
    else {
        control=in[1]; output=out[0]; fcntl(control,F_SETFL,O_NONBLOCK); fcntl(output,F_SETFL,O_NONBLOCK);
        active=selection.selected=index; heartbeat=now(); command('V',volume); strcpy(message,"Opening audio...");
    }
    focus_path=0; dirty=1;
}
static void toggle(void)
{ if(player && active==selection.selected)command('P',!paused); else play(selection.selected); }
static void seek_by(int seconds)
{
    if(!player)return;
    int64_t target=position+(int64_t)seconds*1000; if(target<0)target=0;
    if(duration>=0 && target>duration)target=duration;
    command('S',target); heartbeat=now();
}
static void change_volume(int delta)
{ volume+=delta; if(volume<0)volume=0; if(volume>100)volume=100; command('V',volume); dirty=1; }
static void save(void)
{
    if(loader || !list->ok)return;
    const char *extension=strrchr(path.data,'.');
    if(!extension || (strcasecmp(extension,".m3u") && strcasecmp(extension,".m3u8"))) {
        strcpy(message,"Enter a new .m3u path, then Save M3U"); dirty=1; return;
    }
    size_t size=8;
    for(int i=0;i<list->count;i++)size+=strlen(list->paths[i])+1;
    if(size>65536) { strcpy(message,"Playlist exceeds the 64 KiB save limit"); dirty=1; return; }
    int fd=open(path.data,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) { strcpy(message,"Save needs a new .m3u path; existing files are not replaced"); dirty=1; return; }
    int error=dprintf(fd,"#EXTM3U\n")<0;
    for(int i=0;i<list->count && !error;i++)error=dprintf(fd,"%s\n",list->paths[i])<0;
    if(close(fd))error=1;
    strcpy(message,error?"Playlist write failed; partial file retained":"Playlist saved"); dirty=1;
}
static void line(const char *s)
{
    heartbeat=now(); long long value; unsigned rate,channels; int estimate;
    if(sscanf(s,"INFO %u %u %lld %d",&rate,&channels,&value,&estimate)==4) { duration=value; estimated=estimate; }
    else if(sscanf(s,"POS %lld",&value)==1) { if(position!=value)dirty=1; position=value; }
    else if(!strcmp(s,"STATE playing")) { paused=0; strcpy(message,"Playing"); dirty=1; }
    else if(!strcmp(s,"STATE paused")) { paused=1; strcpy(message,"Paused"); dirty=1; }
    else if(!strcmp(s,"STATE ended")) { ended=1; strcpy(message,"Track finished"); dirty=1; }
    else if(!strcmp(s,"XRUN")) { underruns++; snprintf(message,sizeof(message),"Playing; recovered %d audio underrun(s)",underruns); dirty=1; }
    else if(!strncmp(s,"ERROR ",6)) { failed=1; snprintf(message,sizeof(message),"%.230s",s+6); dirty=1; }
}
static void read_output(void)
{
    if(output<0)return;
    char buffer[1024];
    for(unsigned budget=0;budget<8;budget++) {
        ssize_t n=read(output,buffer,sizeof(buffer)); if(n<=0)break;
        for(ssize_t i=0;i<n;i++) {
            if(buffer[i]=='\n') { incoming[incoming_used]=0; line(incoming); incoming_used=0; }
            else if(incoming_used<sizeof(incoming)-1)incoming[incoming_used++]=buffer[i];
            else { failed=1; strcpy(message,"Invalid worker response"); }
        }
    }
}
static void tick(void)
{
    int status;
    if(loader && waitpid(loader,&status,WNOHANG)==loader) {
        loader=0; list->ok=list->ok && WIFEXITED(status) && WEXITSTATUS(status)==0;
        if(list->ok) { selection.count=list->count; snprintf(message,sizeof(message),"Loaded %d track(s); choose Play",list->count); focus_path=0; }
        else { list->count=0; snprintf(message,sizeof(message),"%s",list->error[0]?list->error:"Playlist load failed"); }
        dirty=1;
    }
    if(loader && now()-load_started>15000) { kill_child(&loader); list->ok=list->count=0; strcpy(message,"Playlist load timed out"); dirty=1; }
    read_output();
    if(player && waitpid(player,&status,WNOHANG)==player) {
        read_output(); player=0;
        int next=active+1,ok=ended && !failed && WIFEXITED(status) && WEXITSTATUS(status)==0;
        stop_player();
        if(ok && next<list->count)play(next);
        else if(ok && repeat)play(0);
        else if(ok)strcpy(message,"Playlist finished");
        else if(!strcmp(message,"Opening audio...") || !strcmp(message,"Playing") || !strcmp(message,"Paused"))strcpy(message,"Audio worker exited unexpectedly");
        dirty=1;
    }
    if(player && now()-heartbeat>15000) { stop_player(); strcpy(message,"Audio worker timed out; playback stopped"); dirty=1; }
}
static int rows(void) { return (height-226)/22; }
static void draw(void)
{
    u.width=width; u.height=height; XClearWindow(u.display,u.window);
    ui_fill(&u,10,10,width-20,26,focus_path?u.accent:u.surface); ui_text_draw(&u,&path,14,15,width-28,16,&top,0);
    ui_button(&u,10,44,72,"Load",0); ui_button(&u,88,44,96,"Save M3U",0);
    ui_button(&u,190,44,72,player&&!paused?"Pause":"Play",player!=0); ui_button(&u,268,44,72,"Stop",0);
    ui_button(&u,346,44,72,"Prev",0); ui_button(&u,424,44,72,"Next",0);
    if(!loader && list->ok) {
        ui_list_reveal(&selection,rows());
        for(int r=0;r<rows() && r+selection.scroll<list->count;r++) {
            int i=r+selection.scroll; const char *name=strrchr(list->paths[i],'/'); char label[PATH_MAX+32];
            snprintf(label,sizeof(label),"%s%3d  %s",i==active?">":" ",i+1,name?name+1:list->paths[i]);
            if(i==selection.selected)ui_fill(&u,10,86+r*22,width-20,22,u.surface);
            ui_label(&u,14,102+r*22,label,u.text);
        }
    }
    char text[160];
    snprintf(text,sizeof(text),"%lld:%02lld / %s%lld:%02lld",(long long)(position/60000),(long long)(position/1000%60),duration<0?"? ":estimated?"~ ":"",(long long)(duration<0?0:duration/60000),(long long)(duration<0?0:duration/1000%60));
    ui_label(&u,12,height-124,text,u.text);
    ui_fill(&u,10,height-108,width-20,12,u.surface);
    if(duration>0) { int fill=(int)((position>duration?duration:position)*(width-20)/duration); ui_fill(&u,10,height-108,fill,12,u.accent); }
    ui_button(&u,10,height-82,60,"-10s",0); ui_button(&u,76,height-82,60,"+10s",0);
    ui_button(&u,154,height-82,38,"-",0); ui_button(&u,262,height-82,38,"+",0);
    snprintf(text,sizeof(text),"%d%%",volume); ui_label(&u,208,height-65,text,u.text);
    ui_button(&u,318,height-82,112,"Repeat",repeat);
    ui_label(&u,12,height-38,"Tab: path/list | Space: pause | F2: save new playlist",u.muted);
    ui_label(&u,12,height-12,message,u.muted);
    char state[128]; snprintf(state,sizeof(state),"track=%d count=%d position=%lld paused=%d volume=%d",active,list->ok?list->count:0,(long long)position,paused,volume);
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_AUDIO_STATUS",False),XA_STRING,8,PropModeReplace,(unsigned char *)message,(int)strlen(message));
    XChangeProperty(u.display,u.window,XInternAtom(u.display,"_WIIDESK_AUDIO_STATE",False),XA_STRING,8,PropModeReplace,(unsigned char *)state,(int)strlen(state));
    XFlush(u.display); dirty=0;
}
static void key(XKeyEvent *e)
{
    KeySym k; char text[32]; int n=XLookupString(e,text,sizeof(text),&k,NULL);
    if(k==XK_Escape) { stop_player(); kill_child(&loader); if(!list->ok)list->count=0; strcpy(message,"Stopped"); dirty=1; return; }
    if(k==XK_Tab) { focus_path=!focus_path; dirty=1; return; }
    if(k==XK_F2) { save(); return; }
    if(k==XK_F5) { load(); return; }
    if(k==XK_Return && (e->state&ControlMask)) { toggle(); return; }
    if((e->state&ControlMask) && (k==XK_Left || k==XK_Right)) { play((active>=0?active:selection.selected)+(k==XK_Left?-1:1)); return; }
    if(loader)return;
    if(focus_path) {
        if(k==XK_Return) { load(); return; }
        if(ui_clipboard_key(&u,&path,k,e->state,e->time)) { paste_until=u.paste_target?now()+3000:0; dirty=1; return; }
        ui_text_key(&path,k,e->state,text,n,0); dirty=1; return;
    }
    if(k==XK_space || k==XK_Return)toggle();
    else if(k==XK_Left || k==XK_Right)seek_by(k==XK_Left?-10:10);
    else if(k==XK_minus)change_volume(-10);
    else if(k==XK_plus || k==XK_equal)change_volume(10);
    else if(k==XK_r || k==XK_R) { repeat=!repeat; dirty=1; }
    else { ui_list_key(&selection,k,rows()); dirty=1; }
}
int main(int argc,char **argv)
{
    if(argc>2 || geteuid()==0) { fprintf(stderr,"Run as desktop user: %s [AUDIO_OR_PLAYLIST]\n",argv[0]); return 1; }
    ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1); if(n<0 || n>=(ssize_t)sizeof(executable)-1)return 1;
    executable[n]=0; char *base=strrchr(executable,'/'); if(!base || base-executable+22>=(ssize_t)sizeof(executable))return 1; strcpy(base+1,"wiidesk-audio-worker");
    u.display=XOpenDisplay(NULL); if(!u.display)return 1;
    list=mmap(NULL,sizeof(*list),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0); if(list==MAP_FAILED)return 1;
    struct preferences prefs; preferences_load(&prefs);
    u.text=color("#eef2f3"); u.muted=color("#aabdc3"); u.surface=color("#333b40"); u.background=color("#242b2f"); u.accent=color(accent_colors[prefs.accent]);
    u.window=XCreateSimpleWindow(u.display,DefaultRootWindow(u.display),20,20,width,height,0,0,u.background); XStoreName(u.display,u.window,"WiiDesk Audio");
    unsigned long pid=getpid(); XChangeProperty(u.display,u.window,XInternAtom(u.display,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,(unsigned char *)&pid,1);
    XClassHint cl={.res_name="audio",.res_class="WiiDesk"}; XSetClassHint(u.display,u.window,&cl);
    XSizeHints hints={.flags=PMinSize,.min_width=540,.min_height=370}; XSetWMNormalHints(u.display,u.window,&hints);
    Atom close_atom=XInternAtom(u.display,"WM_DELETE_WINDOW",False),protocols=XInternAtom(u.display,"WM_PROTOCOLS",False); XSetWMProtocols(u.display,u.window,&close_atom,1);
    XSelectInput(u.display,u.window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask|VisibilityChangeMask);
    u.gc=XCreateGC(u.display,u.window,0,NULL); XFontStruct *font=XLoadQueryFont(u.display,"8x13"); if(!font)font=XLoadQueryFont(u.display,"fixed"); if(font)XSetFont(u.display,u.gc,font->fid);
    ui_clipboard_init(&u); if(ui_text_init(&path,PATH_MAX-1))return 1;
    signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal); signal(SIGPIPE,SIG_IGN); XMapWindow(u.display,u.window);
    if(argc==2) { ui_text_set(&path,argv[1],strlen(argv[1])); load(); }
    while(!stopped) {
        while(XPending(u.display) && !stopped) {
            XEvent e; XNextEvent(u.display,&e); if(ui_clipboard_event(&u,&e)) { dirty=1; continue; }
            if(e.type==Expose)dirty=1;
            else if(e.type==VisibilityNotify) { visible=e.xvisibility.state!=VisibilityFullyObscured; if(visible)dirty=1; }
            else if(e.type==ConfigureNotify) { width=e.xconfigure.width; height=e.xconfigure.height; dirty=1; }
            else if(e.type==ClientMessage && e.xclient.message_type==protocols && (Atom)e.xclient.data.l[0]==close_atom)stopped=1;
            else if(e.type==KeyPress)key(&e.xkey);
            else if(e.type==ButtonPress && e.xbutton.button==1) {
                int x=e.xbutton.x,y=e.xbutton.y;
                focus_path=ui_hit(x,y,10,10,width-20,26);
                if(focus_path)ui_text_click(&path,x-14,0,width-28,top,0,e.xbutton.state&ShiftMask);
                else if(ui_hit(x,y,10,44,72,24))load();
                else if(ui_hit(x,y,88,44,96,24))save();
                else if(ui_hit(x,y,190,44,72,24))toggle();
                else if(ui_hit(x,y,268,44,72,24)) { stop_player(); kill_child(&loader); strcpy(message,"Stopped"); }
                else if(ui_hit(x,y,346,44,72,24))play((active>=0?active:selection.selected)-1);
                else if(ui_hit(x,y,424,44,72,24))play((active>=0?active:selection.selected)+1);
                else if(ui_hit(x,y,10,height-82,60,24))seek_by(-10);
                else if(ui_hit(x,y,76,height-82,60,24))seek_by(10);
                else if(ui_hit(x,y,154,height-82,38,24))change_volume(-10);
                else if(ui_hit(x,y,262,height-82,38,24))change_volume(10);
                else if(ui_hit(x,y,318,height-82,112,24))repeat=!repeat;
                else if(ui_hit(x,y,10,height-108,width-20,12) && duration>0)command('S',(int64_t)(x-10)*duration/(width-20));
                else if(y>=86 && y<86+rows()*22 && !loader && list->ok) { int at=selection.scroll+(y-86)/22; if(at<list->count)selection.selected=at; }
                dirty=1;
            }
        }
        tick(); if(stopped)break;
        if(u.paste_target && now()>paste_until)ui_clipboard_cancel(&u);
        if(dirty && visible)draw();
        struct pollfd fds[2]={{.fd=ConnectionNumber(u.display),.events=POLLIN},{.fd=output,.events=POLLIN}};
        poll(fds,2,player || loader || u.paste_target?100:-1);
    }
    stop_player(); kill_child(&loader); munmap(list,sizeof(*list)); ui_text_free(&path); ui_clipboard_free(&u);
    if(font)XFreeFont(u.display,font);
    XFreeGC(u.display,u.gc); XDestroyWindow(u.display,u.window); XCloseDisplay(u.display); return 0;
}
