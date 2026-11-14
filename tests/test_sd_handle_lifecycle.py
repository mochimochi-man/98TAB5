#!/usr/bin/env python3
"""Compile the production handle cache/lifecycle with a deterministic syscall fixture."""
from pathlib import Path
import tempfile,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'main/hostfile_sd.cpp').read_text(encoding='utf-8')
a=s.index('#define OPEN_CACHE_SLOTS')
b=s.index('// ---- built-in ROMs',a)
production=s[a:b]+'\n}\n'+s[s.index('UINT hf_write('):s.index('short hf_close(')]
prefix=r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#define MAX_PATH 256
#define USE_IO_WORKER 0
using UINT=unsigned; struct RFILE { int fd; }; using HFILE=RFILE*;
enum { IOP_OPEN, IOP_CLOSE, IOP_SYNC, IOP_TRUNC, IOP_WRITE };
struct IoReq { int op=0,fd=0,flags=0; const char *path=nullptr; void *buf=nullptr; size_t len=0; };
static std::vector<int> calls;
static int next_fd=10;
static bool fail_sync, fail_trunc;
static int ets_printf(const char*, ...) { return 0; }
static long io_call(const IoReq &r) {
 if(r.op==IOP_TRUNC) return fail_trunc ? -1 : 0;
 if(r.op==IOP_WRITE) return r.len;
 if(r.op==IOP_OPEN) { int fd=next_fd++; calls.push_back(fd); return fd; }
 calls.push_back((r.op==IOP_SYNC?1000:2000)+r.fd);
 return r.op==IOP_SYNC && fail_sync ? -1 : 0;
}
"""
fixture=r"""
int main() {
 hf_term(); assert(calls.empty());
 int a=cache_open("/sd/a.hdm",true); assert(a==10 && cache_owns(a));
 hf_init(); assert(cache_open("/sd/a.hdm",true)==a); // startup must not orphan a pre-opened file
 int b=cache_open("/sd/b.hdm",true); assert(b==11);
 hf_term();
 assert((calls==std::vector<int>{10,11,1010,2010,1011,2011}));
 assert(!cache_owns(a) && !cache_owns(b));
 hf_term(); assert(calls.size()==6); // safe on repeated unmount
 int c=cache_open("/sd/a.hdm",true); assert(c==12);
 fail_sync=true; hf_term();
 assert(calls[calls.size()-2]==1012 && calls.back()==2012 && !cache_owns(c));
 RFILE file{17}; assert(hf_write(&file,nullptr,0)==0);
 fail_trunc=true; assert(hf_write(&file,nullptr,0)==(UINT)-1);
 puts("PASS: pre-init safety, cached-fd reuse, init preservation, sync-before-close, idempotence, failed-sync cleanup, truncate-error propagation");
}
"""
with tempfile.TemporaryDirectory(prefix='pc98-sd-lifecycle-') as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(prefix+production+fixture)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
