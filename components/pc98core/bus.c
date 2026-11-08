/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * PC-9801 CPU memory router. Address-space contract: DESIGN_CPU.md and
 * MIGRATION_LOG stage 2c memory survey. Device behaviour stays in dev/video.
 * The legacy CPU header is an adapter until the instruction core is replaced.
 */
#include <compiler.h>
#include <cpulink.h>
#include <video.h>

UINT8 mem[0x200000] __attribute__((aligned(16)));
typedef REG8 (*Read8)(UINT32);
typedef REG16 (*Read16)(UINT32);
typedef void (*Write8)(UINT32,REG8);
typedef void (*Write16)(UINT32,REG16);
typedef struct { Read8 r8;Read16 r16;Write8 w8;Write16 w16; } BusPage;
static BusPage pages[34];
static UINT video_mode;
/* the page plain reads and writes of A8000h-BFFFFh go to (no GRCG / EGC), -1 otherwise: the CPU's fast loop
 * takes those itself */
int g_gvram_plain;

/* RAM backing addresses and the high BIOS/device aliases share low storage. */
static UINT32 ram_address(UINT32 a) { return a>=0xf00000 ? a&0xfffff : a; }
static REG8 ram_r8(UINT32 a) { return mem[ram_address(a)]; }
static REG16 ram_r16(UINT32 a) { a=ram_address(a);return mem[a]|((REG16)mem[a+1]<<8); }
static void ram_w8(UINT32 a,REG8 v) { mem[ram_address(a)]=(UINT8)v; }
static void ram_w16(UINT32 a,REG16 v) { a=ram_address(a);mem[a]=(UINT8)v;mem[a+1]=(UINT8)(v>>8); }
static REG8 empty_r8(UINT32 a) { (void)a;return 0xff; }
static REG16 empty_r16(UINT32 a) { (void)a;return 0xffff; }
/* The current machine retains low backing bytes even where reads are open bus.
 * Preserve that observable legacy behaviour while bounding absent high memory. */
static void empty_w8(UINT32 a,REG8 v) { if(a<sizeof(mem) && (g_cpu.d000_ram & (1u<<((a>>12)&15))))mem[a]=(UINT8)v; }
static void empty_w16(UINT32 a,REG16 v) { empty_w8(a,(REG8)v);empty_w8(a+1,(REG8)(v>>8)); }
/* F8000h-FFFFFh: the BIOS bank, or the ITF bank (kept 100000h above it in mem[]) while 043Dh selects it. */
static REG8 rom_r8(UINT32 a) { return mem[(a&0xfffff)+(g_cpu.itf_rom?0x100000:0)]; }
static REG16 rom_r16(UINT32 a) { a=(a&0xfffff)+(g_cpu.itf_rom?0x100000:0);return mem[a]|((REG16)mem[a+1]<<8); }
static const BusPage ram_page={ram_r8,ram_r16,ram_w8,ram_w16};
static const BusPage empty_page={empty_r8,empty_r16,empty_w8,empty_w16};
static const BusPage text_page={tvram_rd8,tvram_rd16,tvram_wr8,tvram_wr16};
/* D0000h-DFFFFh: ROM space, each 4KB block writable only where g_cpu.d000_ram turns RAM on (the same rule as empty_w8). */
static const BusPage dram_page={ram_r8,ram_r16,empty_w8,empty_w16};
static const BusPage plain_video[2]={
 {gvram_rd8_0,gvram_rd16_0,gvram_wr8_0,gvram_wr16_0},
 {gvram_rd8_1,gvram_rd16_1,gvram_wr8_1,gvram_wr16_1}
};
static const BusPage tile_video[2]={
 {grcg_tcr8_0,grcg_tcr16_0,grcg_tdw8_0,grcg_tdw16_0},
 {grcg_tcr8_1,grcg_tcr16_1,grcg_tdw8_1,grcg_tdw16_1}
};
static const BusPage rmw_video[2]={
 {gvram_rd8_0,gvram_rd16_0,grcg_rmw8_0,grcg_rmw16_0},
 {gvram_rd8_1,gvram_rd16_1,grcg_rmw8_1,grcg_rmw16_1}
};
static const BusPage egc_video={egc_rd8,egc_rd16,egc_wr8,egc_wr16};
void bus_set_vram_access(UINT op)
{
 const BusPage *v=&plain_video[op&1];
 video_mode=op;
 if(op&8) {
  if(op&2)v=&egc_video;
  else v=(op&4)?&rmw_video[op&1]:&tile_video[op&1];
 }
 pages[21]=pages[22]=pages[23]=*v;
 g_gvram_plain=(op&8)?-1:(int)(op&1);
 pages[28]=(op&16)?*v:empty_page;
}
/* The machine is a NEC PC-9801: `type` (EPSON memory map) is not supported. C0000h-CFFFFh is plain RAM (no EMS board). */
void bus_set_map(UINT type)
{
 unsigned n;
 (void)type;
 for(n=0;n<34;n++)pages[n]=ram_page;
 pages[20]=text_page;
 pages[26]=pages[27]=dram_page;
 pages[29].w8=pages[30].w8=empty_w8;
 pages[29].w16=pages[30].w16=empty_w16;
 pages[31].r8=rom_r8;pages[31].r16=rom_r16;
 pages[31].w8=empty_w8;pages[31].w16=empty_w16;
 bus_set_vram_access(video_mode);
}
static const BusPage *page_at(UINT32 a)
{
 if(a<HIMEM_END)return &pages[a>>15];
 if(a>=0xfa0000 && a<=0xffffff)return &pages[(a&0xfffff)>>15];
 return &empty_page;
}
/* write mode of graphics VRAM at this address (REP store fast path): bit0 = page, bits1-2 = 0 plain / 1 GRCG TDW / 2 GRCG RMW; -1 = anything else */
int bus_vram_write_mode(UINT32 a)
{
 const BusPage *p=&pages[a>>15];
 if(p->w16==gvram_wr16_0)return 0;
 if(p->w16==gvram_wr16_1)return 1;
 if(p->w16==grcg_tdw16_0)return 2;
 if(p->w16==grcg_tdw16_1)return 3;
 if(p->w16==grcg_rmw16_0)return 4;
 if(p->w16==grcg_rmw16_1)return 5;
 return -1;
}
REG8 bus_rd8(UINT32 a)
{
 const BusPage *p;
 if(a<0xa4000)return mem[a];
 a&=g_cpu.addr_mask;

 if(a>=HIMEM_END && a<g_cpu.ext_end16)return g_cpu.ext[a-0x100000];
 if(a>=0xfa0000 && a<=0xffffff)a&=0xfffff;
 p=page_at(a);return p->r8(a);
}
REG16 bus_rd16(UINT32 a)
{
 const BusPage *p;
 if(a<0xa3fff)return mem[a]|((REG16)mem[a+1]<<8);
 if((a&0x7fff)==0x7fff){REG8 lo=bus_rd8(a);return lo|((REG16)bus_rd8(a+1)<<8);}
 a&=g_cpu.addr_mask;

 if(a>=HIMEM_END && a<g_cpu.ext_end16)return g_cpu.ext[a-0x100000]|((REG16)g_cpu.ext[a+1-0x100000]<<8);
 if(a>=0xfa0000 && a<=0xffffff)a&=0xfffff;
 p=page_at(a);return p->r16(a);
}
void bus_wr8(UINT32 a,REG8 v)
{
 const BusPage *p;
 if(a<0xa0000){mem[a]=(UINT8)v;return;}
 a&=g_cpu.addr_mask;
 if(a>=HIMEM_END && a<g_cpu.ext_end16){g_cpu.ext[a-0x100000]=(UINT8)v;return;}
 if(a>=HIMEM_END && a<0xf00000)return;
 if(a>=0xfa0000 && a<=0xffffff)a&=0xfffff;
 p=page_at(a);p->w8(a,v);
}
void bus_wr16(UINT32 a,REG16 v)
{
 const BusPage *p;
 if(a<0x9ffff){mem[a]=(UINT8)v;mem[a+1]=(UINT8)(v>>8);return;}
 if((a&0x7fff)==0x7fff){bus_wr8(a,(REG8)v);bus_wr8(a+1,(REG8)(v>>8));return;}
 a&=g_cpu.addr_mask;
 if(a>=HIMEM_END && a<g_cpu.ext_end16){g_cpu.ext[a-0x100000]=(UINT8)v;g_cpu.ext[a+1-0x100000]=(UINT8)(v>>8);return;}
 if(a>=HIMEM_END && a<0xf00000)return;
 if(a>=0xfa0000 && a<=0xffffff)a&=0xfffff;
 p=page_at(a);p->w16(a,v);
}
UINT32 bus_rd32(UINT32 a)
{
 if(a<0xa3ffd)return mem[a]|((UINT32)mem[a+1]<<8)|((UINT32)mem[a+2]<<16)|((UINT32)mem[a+3]<<24);
 if(a&1){REG8 lo=bus_rd8(a);REG16 middle=bus_rd16(a+1);return lo|((UINT32)middle<<8)|((UINT32)bus_rd8(a+3)<<24);}
 {REG16 lo=bus_rd16(a);return lo|((UINT32)bus_rd16(a+2)<<16);}
}
void bus_wr32(UINT32 a,UINT32 v)
{
 if(a<0x9fffd){mem[a]=(UINT8)v;mem[a+1]=(UINT8)(v>>8);mem[a+2]=(UINT8)(v>>16);mem[a+3]=(UINT8)(v>>24);return;}
 if(a&1){bus_wr8(a,(REG8)v);bus_wr16(a+1,(REG16)(v>>8));bus_wr8(a+3,(REG8)(v>>24));}
 else{bus_wr16(a,(REG16)v);bus_wr16(a+2,(REG16)(v>>16));}
}
void bus_read_block(UINT32 a,void *dst,UINT n)
{
 UINT8 *d=(UINT8*)dst;
 while(n--)*d++=(UINT8)bus_rd8(a++);
}
void bus_write_block(UINT32 a,const void *src,UINT n)
{
 const UINT8 *s=(const UINT8*)src;
 while(n--)bus_wr8(a++,*s++);
}
