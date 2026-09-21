// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Runs unprivileged in xterm. Password input stays with sudo's controlling tty. */
int main(int argc,char **argv)
{
    if(argc<4 || (argc-2)%2 || argc>34 || geteuid()==0)return 2;
    int directory=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(directory<0 || fstat(directory,&st) || st.st_uid!=getuid() || (st.st_mode&077))return 2;
    int log=openat(directory,"install.log",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(log<0)return 2;
    char helper[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",helper,sizeof(helper)-1);
    if(n<0 || n>=(ssize_t)sizeof(helper)-1)return 2;
    helper[n]=0; char *base=strrchr(helper,'/');
    if(!base || (size_t)(base+1-helper)+strlen("wiidesk-package-install")>=sizeof(helper))return 2;
    strcpy(base+1,"wiidesk-package-install");
    int pipefd[2]; if(pipe2(pipefd,O_CLOEXEC))return 2;
    pid_t child=fork(); if(child<0)return 2;
    if(!child) {
        close(pipefd[0]); dup2(pipefd[1],STDOUT_FILENO); dup2(pipefd[1],STDERR_FILENO); close(pipefd[1]);
        char *args[40]={"/usr/bin/sudo","-k","--",helper};
        for(int i=2;i<argc;i++)args[i+2]=argv[i];
        args[argc+2]=NULL; execv(args[0],args); _exit(127);
    }
    close(pipefd[1]); size_t logged=0; char buffer[4096]; ssize_t got;
    while((got=read(pipefd[0],buffer,sizeof(buffer)))!=0) {
        if(got<0) { if(errno==EINTR)continue; break; }
        size_t sent=0;
        while(sent<(size_t)got) { ssize_t k=write(STDOUT_FILENO,buffer+sent,(size_t)got-sent); if(k<0 && errno==EINTR)continue; if(k<=0)break; sent+=(size_t)k; }
        size_t keep=(size_t)got; if(keep>1024*1024-logged)keep=1024*1024-logged;
        sent=0; while(sent<keep) { ssize_t k=write(log,buffer+sent,keep-sent); if(k<0 && errno==EINTR)continue; if(k<=0)break; sent+=(size_t)k; }
        logged+=sent;
    }
    close(pipefd[0]); if(logged==1024*1024) { const char note[]="\n[Log truncated at 1 MiB]\n"; if(write(log,note,sizeof(note)-1)<0)perror("Log"); }
    close(log); int status; while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 2;
    int rc=WIFEXITED(status)?WEXITSTATUS(status):1;
    int result=openat(directory,"result.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(result<0)return 2;
    if(dprintf(result,"%d\n",rc)<0 || fsync(result)) { close(result); return 2; }
    if(close(result) || renameat(directory,"result.new",directory,"result"))return 2;
    close(directory); return rc;
}
