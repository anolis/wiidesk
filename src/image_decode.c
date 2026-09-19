// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "image_decode.h"
#include <stdio.h>
#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int dimensions(unsigned w,unsigned h)
{ return w && h && w<=4096 && h<=4096 && w<=IMAGE_PIXELS/h; }
static int picture(const char *s)
{
    const char *ext=strrchr(s,'.');
    return ext && (!strcasecmp(ext,".png") || !strcasecmp(ext,".jpg") || !strcasecmp(ext,".jpeg"));
}
static int compare(const void *a,const void *b) { return strcmp(a,b); }
static int neighbor(char *path,int direction,char *error,size_t size)
{
    char folder[PATH_MAX], names[256][NAME_MAX+1];
    strcpy(folder,path); char *base=strrchr(folder,'/');
    if(!base)return -1;
    char current[NAME_MAX+1]; snprintf(current,sizeof(current),"%s",base+1);
    if(base==folder)base[1]=0; else *base=0;
    DIR *d=opendir(folder);
    if(!d) { snprintf(error,size,"Folder: %s",strerror(errno)); return -1; }
    struct dirent *entry; int count=0,seen=0,overflow=0;
    while((entry=readdir(d))) {
        if(++seen>4096) { overflow=1; break; }
        if(!picture(entry->d_name))continue;
        if(count==256) { overflow=1; break; }
        strcpy(names[count++],entry->d_name);
    }
    closedir(d);
    if(overflow) { snprintf(error,size,"Browse limit: 256 pictures / 4096 folder entries"); return -1; }
    qsort(names,(size_t)count,sizeof(names[0]),compare);
    int index=-1;
    for(int i=0;i<count;i++)if(!strcmp(names[i],current))index=i;
    if(index<0 || index+direction<0 || index+direction>=count) {
        snprintf(error,size,"No %s picture",direction>0?"next":"previous"); return -1;
    }
    if(snprintf(path,PATH_MAX,"%s%s%s",folder,!strcmp(folder,"/")?"":"/",names[index+direction])>=PATH_MAX) {
        snprintf(error,size,"Path too long"); return -1;
    }
    return 0;
}
struct jpeg_error { struct jpeg_error_mgr base; jmp_buf jump; char message[JMSG_LENGTH_MAX]; };
struct jpeg_state { struct jpeg_decompress_struct info; struct jpeg_error error; int created; };
static void jpeg_fail(j_common_ptr c)
{
    struct jpeg_error *e=(struct jpeg_error *)c->err;
    (*c->err->format_message)(c,e->message); longjmp(e->jump,1);
}
static void jpeg_message(j_common_ptr c,int level)
{ if(level<0)jpeg_fail(c); } /* Reject truncated inputs rather than invent pixels. */
static void load_jpeg(FILE *f,struct image_result *out)
{
    struct jpeg_state *s=calloc(1,sizeof(*s));
    if(!s) { strcpy(out->error,"Not enough decoder memory"); return; }
    s->info.err=jpeg_std_error(&s->error.base);
    s->error.base.error_exit=jpeg_fail; s->error.base.emit_message=jpeg_message;
    if(setjmp(s->error.jump)) { snprintf(out->error,sizeof(out->error),"JPEG: %.160s",s->error.message); goto done; }
    jpeg_create_decompress(&s->info); s->created=1;
    s->info.mem->max_memory_to_use=2*1024*1024;
    jpeg_stdio_src(&s->info,f); jpeg_read_header(&s->info,TRUE);
    if(!dimensions(s->info.image_width,s->info.image_height)) {
        strcpy(out->error,"Image limit: 1 megapixel; each side at most 4096"); goto done;
    }
    s->info.out_color_space=JCS_RGB;
    jpeg_start_decompress(&s->info);
    out->width=s->info.output_width; out->height=s->info.output_height;
    while(s->info.output_scanline<s->info.output_height) {
        JSAMPROW row=out->rgb+(size_t)s->info.output_scanline*out->width*3;
        if(jpeg_read_scanlines(&s->info,&row,1)!=1) { strcpy(out->error,"Incomplete JPEG"); goto done; }
    }
    jpeg_finish_decompress(&s->info); out->ok=1;
done:
    if(s->created)jpeg_destroy_decompress(&s->info);
    free(s);
}
void image_decode(const char *path,int direction,struct image_result *out)
{
    out->ok=0; out->width=out->height=0; out->error[0]=0;
    if(!realpath(path,out->path)) { snprintf(out->error,sizeof(out->error),"Open: %s",strerror(errno)); return; }
    if(direction && neighbor(out->path,direction,out->error,sizeof(out->error)))return;
    int fd=open(out->path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) { snprintf(out->error,sizeof(out->error),"Open: %s",strerror(errno)); return; }
    struct stat st;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>16*1024*1024) {
        close(fd); strcpy(out->error,"Choose a regular image file, at most 16 MiB"); return;
    }
    FILE *f=fdopen(fd,"rb");
    if(!f) { close(fd); strcpy(out->error,"Could not open image stream"); return; }
    unsigned char magic[8]; size_t n=fread(magic,1,sizeof(magic),f); rewind(f);
    if(n==8 && !png_sig_cmp(magic,0,8)) {
        png_image image={.version=PNG_IMAGE_VERSION};
        if(!png_image_begin_read_from_stdio(&image,f))snprintf(out->error,sizeof(out->error),"PNG: %.160s",image.message);
        else if(!dimensions(image.width,image.height))strcpy(out->error,"Image limit: 1 megapixel; each side at most 4096");
        else {
            image.format=PNG_FORMAT_RGB;
            png_color background={36,43,47};
            if(png_image_finish_read(&image,&background,out->rgb,0,NULL)) {
                out->width=image.width; out->height=image.height; out->ok=1;
            } else snprintf(out->error,sizeof(out->error),"PNG: %.160s",image.message);
        }
        png_image_free(&image);
    } else if(n>=2 && magic[0]==255 && magic[1]==216)load_jpeg(f,out);
    else strcpy(out->error,"Unsupported image: choose PNG or JPEG");
    fclose(f);
}
