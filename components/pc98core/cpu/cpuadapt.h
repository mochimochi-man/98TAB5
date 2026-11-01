/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * The adapter between the CPU (menu 0 V30, 1 286, 2 286 with the 386 instructions) and the machine (machine.h).
 *
 * Shared with the scheduler and the devices through g_cpu (cpulink.h): the slice clocks, the address mask, the
 * reset request, the ITF bank, the D0000h RAM switch, the CPU kind and extended memory. The registers live in
 * g_cpuad and are copied into g_cpu.reg after every slice for diagnostics. */
#ifndef PC98_CPU_ADAPT_H
#define PC98_CPU_ADAPT_H
#include "menu_shut.h"
#include "menu_cpu.h"

typedef struct Pc98Adapter {
    Pc98Mode286 mode;
    Pc98Cpu386State s;
    Pc98ExecuteBus bus;      /* memory / port endpoints */
    Pc98ExecuteBus pbus;     /* prepared 286 bus for the current PE state */
    unsigned pbus_msw;
    unsigned pbus_valid;
    unsigned switched;       /* task_committed sink */
    unsigned menu;           /* 0 V30, 1 286, 2 286 + 386 instructions */
    unsigned active;         /* 1: new CPU owns registers */
    unsigned stopped;        /* unsupported instruction: CPU frozen */
    unsigned pending;        /* latched IRQ vector | 0x100 (blocked by a shadow) */
    unsigned rep_valid;
    uint16_t rep_ip;
    unsigned nfetch;
    uint8_t ib[8];
    uint32_t stop_pc;
    uint8_t stop_bytes[8];
    unsigned long long retired;
    uint8_t *ram;            /* fast16: flat low RAM (firmware: mem[]); NULL disables the fast path */
    unsigned fast_ok;        /* fast16 eligibility, re-evaluated after reference steps */
    const uint8_t *rom;      /* fast16: code may be fetched directly from rom[E8000h..FFFFFh] while *itf == 0 (NULL: bus only) */
    const volatile uint8_t *itf;   /* ITF bank flag (firmware: &g_cpu.itf_rom) */
    uint8_t *hma;            /* fast16: mem + 100000h (HMA direct access while A20 is open); NULL: bus only */
    const volatile uint32_t *addr_mask_p;   /* firmware: &g_cpu.addr_mask */
    /* optional direct device-memory accessors (firmware: memp_*) used instead of the bus for accesses fast16 cannot do inline */
    unsigned (*devrb)(uint32_t);
    unsigned (*devrw)(uint32_t);
    void (*devwb)(uint32_t, unsigned);
    void (*devww)(uint32_t, unsigned);
    int32_t *slice_left_p;       /* fast16 run: &g_cpu.slice_left, decremented by fast16 itself (NULL: caller accounts) */
    uint8_t r8o[8];          /* fast16: byte offset (from this struct) of AL CL DL BL AH CH DH BH; filled lazily by fast16 */
    /* optional: REP store run into graphics VRAM (firmware: vmem_vram_run); vpage returns the VRAM write mode at the address (bit0 page, bits1-2 plain/TDW/RMW) or -1 */
    int (*vpage)(uint32_t);
    unsigned (*vrun)(uint32_t da, int step, unsigned n, unsigned w, const uint8_t *src, uint32_t sa, unsigned val, unsigned mode, int32_t acc0, int32_t acc, int32_t *remclk, int32_t off);
    /* slice end on STI/IRET/POPF with a deliverable IRQ: probe returns pic_pending() (NULL: off) */
    int (*irqprobe)(void);
    unsigned irqend;
    /* fast16 mini loop (fast16_mini.inc): per-model clock tables, a zero halfword for the EA table, and the exit state */
    uint8_t mck_n[256], mck_m[512];
    const uint8_t *mck_main, *mck_t;
    unsigned mck_mi;
    uint16_t mz;
    const uint8_t *mo_pcb, *mck_shift;
    int32_t mo_rem, mo_off;
    unsigned mo_fl, mo_nret, mo_ip, mo_ev, mo_ov, mo_rom;
} Pc98Adapter;
extern Pc98Adapter g_cpuad;

void pc98_adapt_initialize(void);
void pc98_adapt_deinitialize(void);
void pc98_adapt_reset(void);
void pc98_adapt_shut(void);
void pc98_adapt_exec(void);             /* pc98_adapt_exec and pc98_adapt_exec */
void pc98_adapt_interrupt(unsigned vector);
void pc98_adapt_mirror(void);

/* One instruction (REP: one iteration) and its clocks, by the reference CPU. Used by pc98_adapt_exec when the
 * fast path declines. Returns the step result. */
enum Pc98StepResult pc98_adapt_step(unsigned *clocks);

/* Live views for devices that sample the CPU. */
unsigned pc98_adapt_is_ei(void);
unsigned pc98_adapt_ax(void);
unsigned pc98_adapt_msw(void);
#endif
