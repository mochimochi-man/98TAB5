#!/usr/bin/env python3
"""Exercise actual file-backed FDD mount/read/write/eject/swap and unsupported-media rejection.
Run with python3 tests/test_release_media.py; requires gcc (no ESP-IDF needed).
"""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "components/pc98core"
C = r"""
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <hostfile.h>
#include <media/drives.h>
HFILE hf_open(const char *p) { return (HFILE)fopen(p,"rb"); }
HFILE hf_open_rw(const char *p) { return (HFILE)fopen(p,"r+b"); }
short hf_close(HFILE f) { return fclose((FILE*)f); }
long hf_seek(HFILE f,long p,int w) { return fseek((FILE*)f,p,w) ? -1 : ftell((FILE*)f); }
long hf_size(HFILE f) { long p=ftell((FILE*)f); fseek((FILE*)f,0,SEEK_END); long n=ftell((FILE*)f); fseek((FILE*)f,p,SEEK_SET); return n; }
UINT hf_read(HFILE f,void *p,UINT n) { return fread(p,1,n,(FILE*)f); }
UINT hf_write(HFILE f,const void *p,UINT n) { if (!n) { fflush((FILE*)f); return ftruncate(fileno((FILE*)f),ftell((FILE*)f)) ? (UINT)-1 : 0; } return fwrite(p,1,n,(FILE*)f); }
int main(void) {
 assert(drv_fd_mount(0,"disk.HDM")==0 && drv_fd_ready(0));
 fm_disk_t *d=fdd_media_disk(0); fm_geom_t g; fm_disk_geometry(d,&g);
 assert(g.tracks==154 && g.sectors==8 && g.n==3);
 const fm_track_t *t=fm_disk_track(d,0,0); assert(t && t->nsect==8);
 unsigned char a[1024], b[1024]; memset(a,0x5a,sizeof a);
 assert(fm_sector_write(d,t,0,a,sizeof a)==FM_OK); fdd_media_flush_all(); drv_fd_eject(0);
 assert(!drv_fd_ready(0)); assert(drv_fd_mount(0,"disk.HDM")==0);
 d=fdd_media_disk(0); t=fm_disk_track(d,0,0);
 assert(fm_sector_read(d,t,0,b,sizeof b)==sizeof b && !memcmp(a,b,sizeof a));
 drv_fd_swap(0,"disk.HDM"); assert(!drv_fd_ready(0));
 for(int i=0;i<23;i++) { drv_fd_tick(); assert(!drv_fd_ready(0)); }
 drv_fd_tick(); assert(drv_fd_ready(0));
 assert(drv_fd_mount(1,"unsupported.xyz")==-1 && !drv_fd_ready(1));
 assert(drv_fd_mount(1,"unsupported.other")==-1 && !drv_fd_ready(1));
 assert(drv_fd_mount(1,"@DEVICE0")==-1 && !drv_fd_ready(1));
 assert(drv_fd_mount(-1,"disk.HDM")==-1);
 drv_fd_swap(0,NULL); assert(!drv_fd_ready(0));
 puts("PASS: mount/read/write/flush/eject/delayed swap; unsupported media rejected");
}
"""
with tempfile.TemporaryDirectory(prefix="pc98-release-media-") as tmp:
    p=Path(tmp)
    (p/"test.c").write_text(C)
    for name in ("disk.HDM", "unsupported.xyz", "unsupported.other"):
        # Same valid raw size: extension rejection must happen before raw fallback.
        (p/name).write_bytes(bytes(77*2*8*1024))
    sources=["media/drives.c", "media/fdmedia.c", "media/fdm_raw.c", "media/fdm_d88.c", "util/pcstr.c"]
    subprocess.run(["gcc", "-std=gnu99", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-I"+str(CORE), "-I"+str(CORE/"include"), str(p/"test.c"), *[str(CORE/s) for s in sources], "-o", str(p/"test")], check=True)
    subprocess.run([str(p/"test")],cwd=p,check=True)
