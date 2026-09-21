// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "package_io.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char job[]="/var/tmp/wiidesk-package-root-XXXXXX";
static int staged,retain;
static void cleanup(void)
{
    if(!staged || retain)return;
    int dir=open(job,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return;
    for(unsigned i=0;i<PACKAGE_MAX;i++) { char file[32]; snprintf(file,sizeof(file),"%02u.deb",i); unlinkat(dir,file,0); }
    close(dir); rmdir(job);
}
/* No setuid bit. sudo performs authentication; this helper never reads passwords. */
int main(int argc,char **argv)
{
    int check=argc>1 && !strcmp(argv[1],"--check-only"); int first=check?2:1;
    if(geteuid()!=0 || argc<=first || (argc-first)%2 || (argc-first)/2>PACKAGE_MAX) {
        fprintf(stderr,"Use sudo: wiidesk-package-install [--check-only] SHA256 SNAPSHOT ...\n"); return 2;
    }
    const char *uidtext=getenv("SUDO_UID"); char *end; unsigned long value=uidtext?strtoul(uidtext,&end,10):0;
    if(!uidtext || !*uidtext || *end || value<1000 || value>UINT_MAX || !getpwuid((uid_t)value)) {
        fprintf(stderr,"An authenticated ordinary sudo user is required\n"); return 2;
    }
    uid_t owner=(uid_t)value;
    clearenv(); setenv("PATH","/usr/sbin:/usr/bin:/sbin:/bin",1); setenv("HOME","/root",1);
    setenv("LC_ALL","C",1); setenv("TERM","xterm",1); setenv("DPKG_DEB_THREADS_MAX","1",1);
    umask(077); if(chdir("/"))return 2;
    char architecture[128]; char *archargs[]={"/usr/bin/dpkg","--print-architecture",NULL};
    if(package_capture(archargs,architecture,sizeof(architecture),0))return 2;
    architecture[strcspn(architecture,"\r\n")]=0;
    if(!mkdtemp(job)) { perror("Root staging"); return 2; }
    staged=1; if(atexit(cleanup)) { cleanup(); return 2; }
    printf("Verified package staging: %s\n",job); fflush(stdout);
    char files[PACKAGE_MAX][PATH_MAX]; struct package_entry entries[PACKAGE_MAX]; unsigned total=0;
    int count=(argc-first)/2;
    for(int i=0;i<count;i++) {
        const char *hash=argv[first+i*2],*source=argv[first+i*2+1];
        if(strlen(hash)!=64) { fprintf(stderr,"Invalid reviewed hash\n"); return 2; }
        for(int j=0;j<64;j++)if(!isxdigit((unsigned char)hash[j])) { fprintf(stderr,"Invalid reviewed hash\n"); return 2; }
        snprintf(files[i],sizeof(files[i]),"%s/%02d.deb",job,i);
        int fd=open(files[i],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0) { perror("Stage"); return 2; }
        char error[256],actual[65]; unsigned bytes;
        int rc=package_copy(source,fd,owner,PACKAGE_TOTAL_MAX-total,&bytes,error,sizeof(error));
        if(close(fd) && !rc) { strcpy(error,"Cannot close staged package"); rc=-1; }
        if(rc) { fprintf(stderr,"%s\n",error); return 2; }
        total+=bytes;
        if(package_hash(files[i],actual) || strcmp(actual,hash)) { fprintf(stderr,"Package changed since review; nothing installed\n"); return 2; }
        memset(&entries[i],0,sizeof(entries[i]));
        if(package_metadata(files[i],&entries[i],error,sizeof(error))) { fprintf(stderr,"%s\n",error); return 2; }
        if(strcmp(entries[i].arch,"all") && strcmp(entries[i].arch,architecture)) {
            fprintf(stderr,"Wrong package architecture: %s (system %s)\n",entries[i].arch,architecture); return 2;
        }
        for(int j=0;j<i;j++)if(!strcmp(entries[i].name,entries[j].name)) { fprintf(stderr,"Duplicate package\n"); return 2; }
        printf("%s %s (%s)\n",entries[i].name,entries[i].version,entries[i].arch);
    }
    if(check) { puts("Stage verification passed; no packages installed"); return 0; }
    /* From here there is deliberately no UI cancellation or installation timeout. */
    puts("Installing with dpkg. Do not close this terminal during installation."); fflush(stdout);
    char *args[PACKAGE_MAX+3]={"/usr/bin/dpkg","--install"};
    for(int i=0;i<count;i++)args[i+2]=files[i];
    args[count+2]=NULL;
    pid_t child=fork(); if(child<0)return 2;
    if(!child) { execv(args[0],args); _exit(127); }
    retain=1;
    int status; while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
    if(!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr,"dpkg failed or was interrupted. Packages may be unpacked but unconfigured. Staged files: %s\n",job);
        return WIFEXITED(status)?WEXITSTATUS(status):1;
    }
    for(int i=0;i<count;i++) {
        char name[170],text[512]; snprintf(name,sizeof(name),"%s:%s",entries[i].name,entries[i].arch);
        char *query[]={"/usr/bin/dpkg-query","--show","--showformat=${Status}\n${Version}\n",name,NULL};
        char expected[256]; snprintf(expected,sizeof(expected),"install ok installed\n%s\n",entries[i].version);
        if(package_capture(query,text,sizeof(text),0) || strcmp(text,expected)) {
            fprintf(stderr,"Post-install verification failed for %s\n",name); return 1;
        }
    }
    puts("Installed package versions verified.");
    retain=0; return 0;
}
