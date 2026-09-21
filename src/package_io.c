// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "package_io.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t active_child;
static uint64_t milliseconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
void package_cancel(int sig)
{ (void)sig; if(active_child>0)kill(-active_child,SIGKILL); _exit(130); }
int package_capture(char *const argv[],char *text,size_t capacity,int limited)
{
    int pipefd[2]; if(capacity<2 || pipe2(pipefd,O_CLOEXEC))return -1;
    sigset_t mask,old; sigemptyset(&mask); sigaddset(&mask,SIGTERM); sigaddset(&mask,SIGINT);
    sigprocmask(SIG_BLOCK,&mask,&old);
    pid_t child=fork();
    if(!child) {
        setpgid(0,0); prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()==1)_exit(125);
        signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL); sigprocmask(SIG_SETMASK,&old,NULL);
        close(pipefd[0]); dup2(pipefd[1],STDOUT_FILENO); dup2(pipefd[1],STDERR_FILENO); close(pipefd[1]);
        int input=open("/dev/null",O_RDONLY); if(input>=0) { dup2(input,STDIN_FILENO); close(input); }
        if(limited) {
            struct rlimit memory={48*1024*1024,48*1024*1024},cpu={15,15},core={0,0};
            if(setrlimit(RLIMIT_AS,&memory) || setrlimit(RLIMIT_CPU,&cpu) || setrlimit(RLIMIT_CORE,&core))_exit(125);
        }
        setenv("LC_ALL","C",1); setenv("DPKG_DEB_THREADS_MAX","1",1);
        execv(argv[0],argv); _exit(127);
    }
    close(pipefd[1]);
    if(child<0) { close(pipefd[0]); sigprocmask(SIG_SETMASK,&old,NULL); return -1; }
    setpgid(child,child); active_child=child; sigprocmask(SIG_SETMASK,&old,NULL);
    fcntl(pipefd[0],F_SETFL,O_NONBLOCK);
    size_t used=0; int status=0,ended=0,eof=0,failed=0; uint64_t deadline=milliseconds()+30000;
    while(!ended || !eof) {
        char buf[1024]; ssize_t n=read(pipefd[0],buf,sizeof(buf));
        if(n>0) {
            if((size_t)n>capacity-1-used) { failed=1; errno=EOVERFLOW; break; }
            memcpy(text+used,buf,(size_t)n); used+=(size_t)n;
        } else if(!n)eof=1;
        else if(errno!=EAGAIN && errno!=EINTR) { failed=1; break; }
        /* Keep the leader unreaped while descendants hold the pipe, so its
         * process-group ID cannot be reused during cancellation/timeout. */
        if(!ended && eof) {
            sigprocmask(SIG_BLOCK,&mask,NULL);
            if(waitpid(child,&status,WNOHANG)==child) { ended=1; active_child=0; }
            sigprocmask(SIG_SETMASK,&old,NULL);
        }
        if(milliseconds()>deadline) { failed=1; errno=ETIMEDOUT; break; }
        if(!ended || !eof) { struct pollfd p={.fd=eof?-1:pipefd[0],.events=POLLIN}; poll(&p,1,20); }
    }
    int saved=errno;
    if(!ended) { kill(-child,SIGKILL); while(waitpid(child,&status,0)<0 && errno==EINTR) {} }
    active_child=0; close(pipefd[0]); text[used]=0; errno=saved;
    return !failed && WIFEXITED(status) && WEXITSTATUS(status)==0?0:-1;
}
int package_hash(const char *path,char digest[65])
{
    char text[PATH_MAX+100]; char *args[]={"/usr/bin/sha256sum","--",(char *)path,NULL};
    if(package_capture(args,text,sizeof(text),0) || strlen(text)<65)return -1;
    for(int i=0;i<64;i++)if(!isxdigit((unsigned char)text[i]))return -1;
    memcpy(digest,text,64); digest[64]=0; return 0;
}
static int field(char **cursor,char *out,size_t size)
{
    char *p=*cursor,*end=strchr(p,'\n'); if(!end || (size_t)(end-p)>=size)return -1;
    memcpy(out,p,(size_t)(end-p)); out[end-p]=0; *cursor=end+1; return 0;
}
int package_metadata(const char *path,struct package_entry *e,char *error,size_t size)
{
    char text[8192];
    char *args[]={"/usr/bin/dpkg-deb","--show","--showformat=${Package}\n${Version}\n${Architecture}\n${Installed-Size}\n${Depends}\n${Pre-Depends}\n${Description}\n",(char *)path,NULL};
    if(package_capture(args,text,sizeof(text),1)) { snprintf(error,size,"Package inspection failed (invalid, oversized or timed out)"); return -1; }
    char number[32],*p=text;
    if(field(&p,e->name,sizeof(e->name)) || field(&p,e->version,sizeof(e->version)) || field(&p,e->arch,sizeof(e->arch)) ||
       field(&p,number,sizeof(number)) || field(&p,e->depends,sizeof(e->depends)) || field(&p,e->predepends,sizeof(e->predepends)) ||
       field(&p,e->description,sizeof(e->description)))goto invalid;
    if(!e->name[0] || !e->version[0] || !e->arch[0])goto invalid;
    for(const char *n=e->name;*n;n++)if(!((*n>='a'&&*n<='z') || isdigit((unsigned char)*n) || strchr("+.-",*n)))goto invalid;
    if(!isalnum((unsigned char)e->name[0]))goto invalid;
    char *end; errno=0; unsigned long kib=strtoul(number,&end,10);
    if(errno==ERANGE || *end || kib>UINT_MAX || number[0]=='-')goto invalid;
    e->installed_kib=(unsigned)kib;
    for(char *s=e->version;*s;s++)if((unsigned char)*s<33 || (unsigned char)*s>126)goto invalid;
    for(char *s=e->arch;*s;s++)if(!isalnum((unsigned char)*s) && *s!='-')goto invalid;
    snprintf(e->installed,sizeof(e->installed),"Not installed / unavailable");
    char qualified[170]; snprintf(qualified,sizeof(qualified),"%s:%s",e->name,e->arch);
    char *query[]={"/usr/bin/dpkg-query","--show","--showformat=${Status}\n${Version}\n",qualified,NULL};
    if(!package_capture(query,text,sizeof(text),0)) {
        char state[128],version[128]; p=text;
        if(!field(&p,state,sizeof(state)) && !field(&p,version,sizeof(version)))
            snprintf(e->installed,sizeof(e->installed),"%.48s | %.64s",state,version);
    }
    return 0;
invalid:
    snprintf(error,size,"Invalid or overlong package metadata"); return -1;
}
int package_copy(const char *path,int output,uid_t owner,unsigned limit,unsigned *size,char *error,size_t error_size)
{
    if(limit>PACKAGE_FILE_MAX)limit=PACKAGE_FILE_MAX;
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC); struct stat before,after;
    if(fd<0)goto fail;
    if(fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_size<1 || (uint64_t)before.st_size>limit ||
       (owner!=(uid_t)-1 && (before.st_uid!=owner || before.st_nlink!=1))) { errno=EINVAL; goto fail; }
    unsigned used=0; char buffer[16384];
    for(;;) {
        ssize_t n=read(fd,buffer,sizeof(buffer)); if(n<0 && errno==EINTR)continue;
        if(n<0)goto fail;
        if(!n)break;
        if((unsigned)n>limit-used) { errno=EFBIG; goto fail; }
        size_t sent=0;
        while(sent<(size_t)n) {
            ssize_t k=write(output,buffer+sent,(size_t)n-sent);
            if(k<0 && errno==EINTR)continue;
            if(k<=0)goto fail;
            sent+=(size_t)k;
        }
        used+=(unsigned)n;
    }
    if(fstat(fd,&after) || before.st_size!=(off_t)used || after.st_size!=before.st_size ||
       after.st_mtim.tv_sec!=before.st_mtim.tv_sec || after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec) { errno=ESTALE; goto fail; }
    close(fd); *size=used; return 0;
fail:;
    int saved=errno; if(fd>=0)close(fd); snprintf(error,error_size,"Copy failed: %s",strerror(saved)); return -1;
}
static int compare(const void *a,const void *b) { return strcmp(a,b); }
void package_inspect(const char *path,struct package_result *r)
{
    char resolved[PATH_MAX],files[PACKAGE_MAX][PATH_MAX]; unsigned count=0,total=0;
    r->ok=0; atomic_store(&r->count,0); r->error[0]=0;
    if(!realpath(path,resolved)) { snprintf(r->error,sizeof(r->error),"Open: %s",strerror(errno)); return; }
    struct stat st; if(lstat(resolved,&st))return;
    if(S_ISDIR(st.st_mode)) {
        DIR *d=opendir(resolved); if(!d)return;
        struct dirent *de; unsigned scanned=0;
        while((de=readdir(d))) {
            if(++scanned>4096) { strcpy(r->error,"Folder limit: 4096 entries"); break; }
            size_t len=strlen(de->d_name);
            if(len<4 || strcasecmp(de->d_name+len-4,".deb"))continue;
            if(count==PACKAGE_MAX) { strcpy(r->error,"Bundle limit: 16 packages"); break; }
            if(snprintf(files[count],PATH_MAX,"%s/%s",resolved,de->d_name)>=PATH_MAX) { strcpy(r->error,"Path too long"); break; }
            count++;
        }
        closedir(d); if(r->error[0])return;
        qsort(files,count,sizeof(files[0]),compare);
    } else { strcpy(files[count++],resolved); }
    if(!count) { strcpy(r->error,"No .deb files found"); return; }
    for(unsigned i=0;i<count;i++) {
        struct package_entry *e=&r->entries[i];
        if(snprintf(e->file,sizeof(e->file),"%s/%02u.deb",r->spool,i)>=(int)sizeof(e->file))return;
        int fd=open(e->file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0) { strcpy(r->error,"Cannot create private package snapshot"); return; }
        int rc=package_copy(files[i],fd,(uid_t)-1,PACKAGE_TOTAL_MAX-total,&e->size,r->error,sizeof(r->error));
        if(close(fd) && !rc)rc=-1;
        if(rc)return;
        if(e->size>PACKAGE_TOTAL_MAX-total) { strcpy(r->error,"Bundle limit: 64 MiB"); return; }
        total+=e->size;
        if(package_hash(e->file,e->digest)) { strcpy(r->error,"Cannot hash package snapshot"); return; }
        if(package_metadata(e->file,e,r->error,sizeof(r->error)))return;
        for(unsigned j=0;j<i;j++)if(!strcmp(e->name,r->entries[j].name)) { strcpy(r->error,"Duplicate package in bundle"); return; }
        char details[PATH_MAX];
        if(snprintf(details,sizeof(details),"%s/metadata-%02u.txt",r->spool,i)>=(int)sizeof(details))return;
        int info=open(details,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(info<0) { strcpy(r->error,"Cannot save package details"); return; }
        int written=dprintf(info,"Package: %s\nVersion: %s\nArchitecture: %s\nInstalled: %s\nInstalled-Size: %u KiB\nDepends: %s\nPre-Depends: %s\nDescription: %s\nSHA256: %s\n",e->name,e->version,e->arch,e->installed,e->installed_kib,e->depends,e->predepends,e->description,e->digest);
        int closed=close(info); if(written<0 || closed) { strcpy(r->error,"Cannot save package details"); return; }
        atomic_store(&r->count,i+1);
    }
    r->ok=1;
}
