/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Host platform layer for the whole-machine harness (stage 7c-2): plain POSIX
 * file I/O, null audio, a dummy frame surface. Mirrors the contracts of
 * main/dosio_sd.cpp and main/platform_stubs.cpp; nothing here is firmware. */
#include <compiler.h>
#include <hostfile.h>
#include <host.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

struct RFILE { int fd; const unsigned char *blob; size_t len, pos; };

/* Host SD root and the two built-in ROMs come from the environment. */
static const char *sd_root(void){ const char *r=getenv("PC98_SD"); return r?r:"."; }
static unsigned char *g_blob[3]; static size_t g_blob_len[3];
static const unsigned char *builtin(const char *path,size_t *len)
{
    const char *env; int idx;
    if(!strcmp(path,":builtin/BIOS.ROM")){idx=0;env="PC98_BIOS";}
    else if(!strcmp(path,":builtin/FONT.ROM")){idx=1;env="PC98_FONT";}
    else if(!strcmp(path,":builtin/HDDROM.BIN")){idx=2;env="PC98_HDDROM";}
    else return NULL;
    if(!g_blob[idx]){
        const char *name=getenv(env);FILE *f=name?fopen(name,"rb"):NULL;
        if(!f){fprintf(stderr,"host: %s not set or unreadable\n",env);return NULL;}
        fseek(f,0,SEEK_END);g_blob_len[idx]=(size_t)ftell(f);fseek(f,0,SEEK_SET);
        g_blob[idx]=malloc(g_blob_len[idx]);
        if(fread(g_blob[idx],1,g_blob_len[idx],f)!=g_blob_len[idx]){fclose(f);return NULL;}
        fclose(f);
    }
    *len=g_blob_len[idx];return g_blob[idx];
}
static void map_path(char *out,size_t n,const char *path)
{
    if(path&&path[0]=='/')snprintf(out,n,"%s%s",sd_root(),path);
    else snprintf(out,n,"%s/%s",sd_root(),path?path:"");
}
static HFILE open_flags(const char *path,int flags)
{
    size_t len=0;const unsigned char *b=builtin((const char*)path,&len);
    struct RFILE *h=calloc(1,sizeof(*h));
    if(b){h->fd=-1;h->blob=b;h->len=len;return (HFILE)h;}
    char full[MAX_PATH+256];map_path(full,sizeof(full),path);
    h->fd=open(full,flags,0644);
    if(h->fd<0){free(h);return NULL;}
    return (HFILE)h;
}
void hf_init(void){}
void hf_term(void){}
HFILE hf_open_rw(const char *p){return open_flags(p,O_RDWR);}
HFILE hf_open(const char *p){return open_flags(p,O_RDONLY);}
HFILE hf_create(const char *p){return open_flags(p,O_RDWR|O_CREAT|O_TRUNC);}
long hf_seek(HFILE handle,long pointer,int method)
{
    struct RFILE *h=(struct RFILE*)handle;if(!h)return 0;
    if(h->fd<0){long p=method==HF_SEEK_CUR?(long)h->pos+pointer:method==HF_SEEK_END?(long)h->len+pointer:(long)pointer;
        if(p<0)p=0;if(p>(long)h->len)p=(long)h->len;h->pos=(size_t)p;return (long)p;}
    off_t r=lseek(h->fd,pointer,method==HF_SEEK_CUR?SEEK_CUR:method==HF_SEEK_END?SEEK_END:SEEK_SET);
    return (long)(r<0?0:r);
}
UINT hf_read(HFILE handle,void *data,UINT length)
{
    struct RFILE *h=(struct RFILE*)handle;if(!h)return 0;
    if(h->fd<0){size_t n=h->len-h->pos;if(n>length)n=length;memcpy(data,h->blob+h->pos,n);h->pos+=n;return (UINT)n;}
    UINT done=0;while(done<length){ssize_t n=read(h->fd,(char*)data+done,length-done);if(n<=0)break;done+=(UINT)n;}
    return done;
}
UINT hf_write(HFILE handle,const void *data,UINT length)
{
    struct RFILE *h=(struct RFILE*)handle;if(!h||h->fd<0)return 0;
    if(!length){if(ftruncate(h->fd,lseek(h->fd,0,SEEK_CUR))){}return 0;}
    UINT done=0;while(done<length){ssize_t n=write(h->fd,(const char*)data+done,length-done);if(n<=0)break;done+=(UINT)n;}
    return done;
}
short hf_close(HFILE handle){struct RFILE *h=(struct RFILE*)handle;if(!h)return -1;if(h->fd>=0)close(h->fd);free(h);return 0;}
long hf_size(HFILE handle)
{
    struct RFILE *h=(struct RFILE*)handle;struct stat st;if(!h)return 0;
    if(h->fd<0)return (long)h->len;
    return fstat(h->fd,&st)?0:(long)st.st_size;
}
short hf_delete(const char *p){char f[MAX_PATH+256];map_path(f,sizeof(f),p);return (short)unlink(f);}
short hf_attr(const char *p){char f[MAX_PATH+256];struct stat st;map_path(f,sizeof(f),p);if(stat(f,&st))return -1;return S_ISDIR(st.st_mode)?HF_ATTR_DIR:HF_ATTR_FILE;}

/* ---- diagnostics / platform ---- */
int ets_printf(const char *fmt,...)
{
    va_list ap;int n;
    if(!getenv("PC98_VERBOSE"))return 0;
    va_start(ap,fmt);n=vfprintf(stderr,fmt,ap);va_end(ap);return n;
}
char g_bios_file[40]="";
void lamp_fdd(REG8 drv){(void)drv;}
void lamp_hdd(REG8 drv){(void)drv;}
void host_fdd_access(UINT8 d){(void)d;}
void host_hdd_access(UINT8 d){(void)d;}
/* a fixed clock keeps repeated runs bit-identical */
UINT32 host_ms(void){return 0;}
static UINT16 s_fb[640*480];
UINT16 *host_frame(int *w,int *h,int *stride){*w=640;*h=480;*stride=640*2;return s_fb;}
/* Fixed wall clock keeps repeated runs bit-identical. */
int host_date(HostDate *d)
{
    d->year=2026;d->month=10;d->day=19;d->hour=12;d->minute=0;d->second=0;
    return 1;
}
UINT8 host_mouse(SINT16 *dx,SINT16 *dy){*dx=0;*dy=0;return 0xa0;}
UINT8 host_joypad(void){return 0xff;}
