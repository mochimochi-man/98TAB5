/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pc98core ROM loading: the only BIOS code that runs is ROM code.
 */
#include <compiler.h>
#include <hostfile.h>
#include <cpulink.h>
#include <pc98.h>
extern int ets_printf(const char *fmt, ...);
#include "rom.h"

#define ROM_BUILTIN   ":builtin/BIOS.ROM"
#define ROM_BASE      0x0e8000u    /* E8000h: start of the BIOS ROM window */
#define ROM_SIZE      0x18000u     /* E8000h-FFFFFh */
#define ITF_SRC       0x0f8000u    /* F8000h-FFFFFh doubles as the ITF image */
#define ITF_SIZE      0x08000u

extern char g_bios_file[40];       /* main.cpp; "" = use the built-in ROM */

/* Read exactly ROM_SIZE bytes of the named file into the ROM window. */
static int read_rom(const char *path)
{
    HFILE fh = hf_open(path);
    int ok = 0;

    if (fh != NULL) {
        ok = (hf_read(fh, mem + ROM_BASE, ROM_SIZE) == (UINT)ROM_SIZE);
        hf_close(fh);
    }
    ets_printf("biosrom: %s -> %s\n", path, ok ? "loaded" : "REJECTED/absent");
    return ok;
}


/* A dumped BIOS has no hard disk code either (on the machine it is in the interface board's ROM): the project's own
 * IDE option ROM (rom_src/hddrom) goes to D8000h, where the BIOS's option ROM scan finds it.  Its 4KB block is made
 * writable: the ROM keeps its variables at the end of the block. */
#define HDDROM_ADRS   0x0d8000u
#define HDDROM_SIZE   0x1000u
static void hddrom_load(void)
{
    HFILE fh = hf_open(":builtin/HDDROM.BIN");
    int ok = 0;

    if (fh != NULL) {
        ok = (hf_read(fh, mem + HDDROM_ADRS, HDDROM_SIZE) == HDDROM_SIZE);
        hf_close(fh);
    }
    if (ok) {
        g_cpu.d000_ram |= (UINT16)(1u << ((HDDROM_ADRS >> 12) & 15));
    }
    ets_printf("biosrom: IDE option ROM at %05X -> %s\n", (unsigned)HDDROM_ADRS, ok ? "loaded" : "absent");
}

/* A BIOS dumped from a real machine (BIOS bank only) has no ITF in it. Its reset
 * vector (FFFF0h) is "JMP FAR FD80:0000" and the code there is the shutdown
 * dispatcher: with port C (35h) bit 7 set and bit 5 (SHUT0) set it stores
 * "OUT DX,AL / JMP FFFF:0000" in RAM, selects the ITF bank (043Dh <- 10h) and jumps
 * to the ITF reset vector; only when the ITF has cleared SHUT0 and switched back
 * (043Dh <- 12h) does the same entry run the BIOS initialisation. With the ITF
 * bank a copy of the BIOS bank, that hand-off never ends: the machine spins at
 * FD80:0000 and nothing is ever drawn. So give the ITF bank the one thing the
 * dispatcher waits for - an original minimal ITF (not NEC code) that clears SHUT0,
 * returns to the BIOS bank through the same RAM trampoline and re-enters the
 * dispatcher. Applied only to an image of exactly that shape; the built-in ROM and
 * ROMs with their own reset vector keep the plain copy. */
static void itf_stub_for_dumped_bios(void)
{
    static const uint8_t pre[7] = { 0xfa, 0x33, 0xc0, 0x8e, 0xd8, 0xe4, 0x35 };
    uint8_t *rv = mem + ITF_AT + 0x7ff0;      /* ITF copy of FFFF0h */
    uint8_t *code = mem + ITF_AT + 0x7f00;    /* F000:FF00h, free in the dumped BIOS */
    unsigned off, seg;
    uint32_t tgt;
    uint8_t stub[] = {
        0xfa,                                   /* cli */
        0x33, 0xc0, 0x8e, 0xd8,                 /* xor ax,ax / mov ds,ax */
        0xb0, 0x0a, 0xe6, 0x37,                 /* mov al,0Ah / out 37h,al : port C bit 5 (SHUT0) <- 0 */
        /* The ITF's memory check is what reports the memory size: the BIOS bank never
         * sizes it, and DOS then sees 128KB. Main memory 640KB, extended memory as
         * configured (none on a V30, told apart the way the built-in ROM does). */
        0x80, 0x26, 0x01, 0x05, 0xf8,           /* and byte [0501h],F8h */
        0x80, 0x0e, 0x01, 0x05, 0x04,           /* or  byte [0501h],04h    : 640KB */
        0x9c, 0x33, 0xc0, 0x50, 0x9d,           /* pushf / xor ax,ax / push ax / popf */
        0x9c, 0x58, 0x9d,                       /* pushf / pop ax / popf */
        0xb3, 0x00,                             /* mov bl,ext (128KB units) */
        0x80, 0xe4, 0xf0, 0x80, 0xfc, 0xf0,     /* and ah,F0h / cmp ah,F0h : V30 keeps FLAGS 12-15 set */
        0x75, 0x02, 0x32, 0xdb,                 /* jne +2 / xor bl,bl */
        0x88, 0x1e, 0x01, 0x04,                 /* mov [0401h],bl */
        0xc7, 0x06, 0xf8, 0x04, 0xee, 0xea,     /* mov [04F8h],EAEEh       : OUT DX,AL / JMP FAR */
        0xc7, 0x06, 0xfa, 0x04, 0x00, 0x00,     /* mov [04FAh],off */
        0xc7, 0x06, 0xfc, 0x04, 0x00, 0x00,     /* mov [04FCh],seg */
        0xba, 0x3d, 0x04,                       /* mov dx,043Dh */
        0xb0, 0x12,                             /* mov al,12h              : BIOS bank */
        0xea, 0xf8, 0x04, 0x00, 0x00            /* jmp 0000:04F8h */
    };
    if (rv[0] != 0xea) return;
    off = rv[1] | (rv[2] << 8);
    seg = rv[3] | (rv[4] << 8);
    tgt = ((uint32_t)seg << 4) + off;
    if (tgt < ROM_BASE || tgt + sizeof(pre) > 0x100000u) return;
    if (memcmp(mem + tgt, pre, sizeof(pre)) != 0) return;
    stub[28] = (uint8_t)(g_mach.ext_mb * 8u < 112u ? g_mach.ext_mb * 8u : 112u);
    stub[19 + 34] = (uint8_t)off; stub[20 + 34] = (uint8_t)(off >> 8);   /* +34: the memory-size block */
    stub[25 + 34] = (uint8_t)seg; stub[26 + 34] = (uint8_t)(seg >> 8);
    memcpy(code, stub, sizeof(stub));
    rv[0] = 0xea;                               /* jmp far F000:FF00h (FFFF:0000h and F000:FFF0h both land here) */
    rv[1] = 0x00; rv[2] = 0xff; rv[3] = 0x00; rv[4] = 0xf0;
    ets_printf("biosrom: dumped BIOS without ITF -> minimal ITF stub (reset %04x:%04x)\n", seg, off);
    hddrom_load();
}

int pc98_rom_load(void)
{
    char path[MAX_PATH];
    int ok = 0;

    if (g_bios_file[0]) {
        /* The menu stores names as picked from the SD root ("/NAME.ROM"). */
        const char *name = g_bios_file + (g_bios_file[0] == '/' ? 1 : 0);
        rom_path(path, (const char *)name, NELEMENTS(path));
        ok = read_rom(path);
    }
    if (!ok) {
        /* Nothing chosen, or the chosen file is missing / wrong size. */
        ok = read_rom((const char *)ROM_BUILTIN);
    }
    if (!ok) {
        return -1;
    }

    g_mach.rom_bios = 1;

    /* ITF bank: same F8000h-FFFFFh image, so switching to it changes nothing.
     * The extended-bank mirrors hold the whole window as well. */
    memcpy(mem + ITF_AT, mem + ITF_SRC, ITF_SIZE);
    itf_stub_for_dumped_bios();
    memcpy(mem + 0x1c0000, mem + ITF_AT, ITF_SIZE);
    memcpy(mem + 0x1e8000, mem + ROM_BASE, 0x10000);
    return 0;
}
