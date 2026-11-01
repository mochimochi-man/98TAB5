/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Firmware adapter for the new CPU, stage 7c-1. See cpuadapt.h and DESIGN_CPU.md
 * "段階7c". Clock debits follow the manufacturers' manuals (clock_tables.inc, gen_clock_tables.py). */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <devices.h>
#include <io.h>
#include "cpuadapt.h"
#include "fast16.h"
#include "purge.h"
#include "menu_reset.h"
#include "real286.h"
#include "mode286.h"
#include <string.h>

extern int ets_printf(const char *fmt, ...);

#ifdef PC98_PROF
#include "cpuprof.h"
void pc98_prof_pre(const Pc98Adapter *A);
void pc98_prof_post(const Pc98Adapter *A, unsigned path);
void pc98_prof_port(unsigned out, unsigned port);
void pc98_prof_irq(unsigned vec);
void pc98_prof_stop(void);
void pc98_prof_fault(unsigned vec, int trap);
#define PROFH(x) x
#else
#define PROFH(x) ((void)0)
#endif
#include "clock_tables.inc"
#include "clock_tables386.inc"

Pc98Adapter g_cpuad;
#ifdef PC98_PROF
#define PROF_ONE 1
#else
#define PROF_ONE 0
#endif

/* ---- bus: straight onto bus.c / iobus.c; RAM below A0000h stays inline ---- */
static uint8_t rd8(void *c, uint32_t a) { (void)c; return a < 0xa4000u ? mem[a] : (uint8_t)bus_rd8(a); }
static uint16_t rd16(void *c, uint32_t a)
{
    (void)c;
    if (a < 0xa3fffu) return (uint16_t)(mem[a] | ((unsigned)mem[a + 1] << 8));
    return (uint16_t)bus_rd16(a);
}
static void wr8(void *c, uint32_t a, uint8_t v) { (void)c; if (a < 0xa0000u) mem[a] = v; else bus_wr8(a, v); }
static void wr16(void *c, uint32_t a, uint16_t v)
{
    (void)c;
    if (a < 0x9ffffu) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); }
    else bus_wr16(a, v);
}
/* Instruction fetch also records the bytes for the clock classifier. */
static uint8_t fetch8(void *c, uint32_t a)
{
    Pc98Adapter *A = c;
    uint8_t v = a < 0xa4000u ? mem[a] : (uint8_t)bus_rd8(a);
    if (A->nfetch < 8) A->ib[A->nfetch] = v;
    A->nfetch++;
    return v;
}
static uint8_t in8(void *c, uint16_t p) { (void)c; PROFH(pc98_prof_port(0, p)); return (uint8_t)io_in8(p); }
static uint16_t in16(void *c, uint16_t p) { (void)c; PROFH(pc98_prof_port(0, p)); return (uint16_t)io_in16(p); }
static void out8(void *c, uint16_t p, uint8_t v) { (void)c; PROFH(pc98_prof_port(1, p)); io_out8(p, v); }
static void out16(void *c, uint16_t p, uint16_t v) { (void)c; PROFH(pc98_prof_port(1, p)); io_out16(p, v); }
/* Instructions complete between DMA ticks, so the bus window is always atomic. */
static void lock_bus(void *c, unsigned on) { (void)c; (void)on; }

int bus_vram_write_mode(uint32_t a);
unsigned vram_store_run(uint32_t da, int step, unsigned n, unsigned w, const uint8_t *src, uint32_t sa, unsigned val, unsigned page, int32_t acc0, int32_t acc, int32_t *remclk, int32_t off);
static unsigned d_rb(uint32_t a) { return (unsigned)bus_rd8(a); }
static unsigned d_rw(uint32_t a) { return (unsigned)bus_rd16(a); }
static void d_wb(uint32_t a, unsigned v) { bus_wr8(a, (REG8)v); }
static void d_ww(uint32_t a, unsigned v) { bus_wr16(a, (REG16)v); }

static void bind_bus(Pc98Adapter *A)
{
    memset(&A->bus, 0, sizeof(A->bus));
    A->bus.context = A;
    A->bus.npx_absent = 1;   /* no FPU: ESC retires, V30 WAIT is a no-op */
    A->bus.read8 = rd8; A->bus.read16 = rd16; A->bus.write8 = wr8; A->bus.write16 = wr16;
    A->bus.in8 = in8; A->bus.in16 = in16; A->bus.out8 = out8; A->bus.out16 = out16;
    A->bus.fetch8 = fetch8; A->bus.fetch_context = A;
    A->bus.lock_bus = lock_bus;
    A->bus.single_step = 1;
    A->pbus_valid = 0;
}

/* ---- lifecycle ---- */
static unsigned menu_of_type(void)
{
    if (g_cpu.v30 & 1) return 0;
    return g_cpu.x386.enabled ? 2u : 1u;
}
static int irq_probe(void) { return pic_pending() ? 1 : 0; }
#ifdef IRQ_MEASURE
unsigned long long g_irq_hist[8]; unsigned long long g_irq_n, g_irq_sum; int32_t g_irq_max;
static void irq_rec(int32_t rem)
{
    unsigned b = rem <= 0 ? 0 : rem < 100 ? 1 : rem < 300 ? 2 : rem < 1000 ? 3 : rem < 3000 ? 4 : rem < 10000 ? 5 : 6;
    g_irq_hist[b]++; g_irq_n++; if (rem > 0) g_irq_sum += (unsigned)rem; if (rem > g_irq_max) g_irq_max = rem;
}
#ifdef IRQ_MEASURE_FIX
#define IRQ_END_SLICE() do { ev_cut_slice(); irq_rec(g_cpu.slice_left); } while (0)
#else
#define IRQ_END_SLICE() do { irq_rec(g_cpu.slice_left); } while (0)
#endif
#include <stdio.h>
__attribute__((destructor)) static void irq_report(void)
{
    fprintf(stderr, "IRQLAT n=%llu avg=%llu max=%d hist=%llu/%llu/%llu/%llu/%llu/%llu/%llu", g_irq_n, g_irq_n ? g_irq_sum / g_irq_n : 0ull, g_irq_max,
        g_irq_hist[0], g_irq_hist[1], g_irq_hist[2], g_irq_hist[3], g_irq_hist[4], g_irq_hist[5], g_irq_hist[6]); fputc(10, stderr);
}
#elif defined(IRQ_NOFIX)
#define IRQ_END_SLICE() do { } while (0)
#else
#define IRQ_END_SLICE() ev_cut_slice()
#endif
void pc98_adapt_initialize(void)
{
    cpu_link_init();
    memset(&g_cpuad, 0, sizeof(g_cpuad));
}
void pc98_adapt_deinitialize(void) { cpu_link_term(); }
static void adopt(Pc98Adapter *A, int cold)
{
    A->menu = menu_of_type();
    A->active = 1;
    A->stopped = 0; A->pending = 0; A->rep_valid = 0; A->nfetch = 0; A->pbus_valid = 0;
    A->ram = (uint8_t *)mem; A->fast_ok = 0;
    A->rom = (const uint8_t *)mem; A->itf = (const volatile uint8_t *)&g_cpu.itf_rom;
    A->hma = (uint8_t *)mem + 0x100000; A->addr_mask_p = (const volatile uint32_t *)&g_cpu.addr_mask; A->slice_left_p = (int32_t *)&g_cpu.slice_left;
    A->irqprobe = irq_probe; A->irqend = 0;
    A->devrb = d_rb; A->devrw = d_rw; A->devwb = d_wb; A->devww = d_ww;
    A->vpage = bus_vram_write_mode; A->vrun = vram_store_run;
    bind_bus(A);
    if (cold) (void)pc98_cpu_menu_reset(A->menu, &A->mode, &A->s);
    else (void)pc98_cpu_menu_shut(A->menu, &A->mode, &A->s);
    pc98_adapt_mirror();
}
void pc98_adapt_reset(void)
{
    cpu_link_reset();       /* shared state (cpustate.c): clocks and g_cpu.v30 are kept; the CPU itself is adopted below */
    adopt(&g_cpuad, 1);
}
void pc98_adapt_shut(void)
{
    cpu_link_shut();
    adopt(&g_cpuad, 0);
}
void pc98_adapt_mirror(void)
{
    const Pc98Cpu16State *c = &g_cpuad.s.low;
    if (!g_cpuad.active) return;
    g_cpu.reg.ax = c->r[PC98_AX]; g_cpu.reg.cx = c->r[PC98_CX]; g_cpu.reg.dx = c->r[PC98_DX]; g_cpu.reg.bx = c->r[PC98_BX];
    g_cpu.reg.sp = c->r[PC98_SP]; g_cpu.reg.bp = c->r[PC98_BP]; g_cpu.reg.si = c->r[PC98_SI]; g_cpu.reg.di = c->r[PC98_DI];
    g_cpu.reg.es = c->seg[PC98_ES]; g_cpu.reg.cs = c->seg[PC98_CS]; g_cpu.reg.ss = c->seg[PC98_SS]; g_cpu.reg.ds = c->seg[PC98_DS];
    g_cpu.reg.es_base = c->base[PC98_ES]; g_cpu.reg.cs_base = c->base[PC98_CS]; g_cpu.reg.ss_base = c->base[PC98_SS]; g_cpu.reg.ds_base = c->base[PC98_DS];
    g_cpu.reg.ip = c->ip; g_cpu.reg.flags = c->flags;
    g_cpu.reg.msw = (UINT16)g_cpuad.mode.system.msw;
    if (g_cpuad.menu == 2) {
        unsigned i;
        for (i = 0; i < 8; i++) g_cpu.x386.hi[i] = g_cpuad.s.high[i];
        g_cpu.x386.fs = g_cpuad.s.extra[0].selector; g_cpu.x386.fs_base = g_cpuad.s.extra[0].base;
        g_cpu.x386.gs = g_cpuad.s.extra[1].selector; g_cpu.x386.gs_base = g_cpuad.s.extra[1].base;
        g_cpu.x386.flags_hi = (UINT16)(c->flags & 0x7000u);
    }
}
unsigned pc98_adapt_is_ei(void)
{
    return (g_cpuad.s.low.flags & 0x200u) != 0;
}
unsigned pc98_adapt_ax(void) { return g_cpuad.s.low.r[PC98_AX]; }
unsigned pc98_adapt_msw(void) { return g_cpuad.mode.system.msw; }

/* ---- timing (old-compatible profile) ---- */
#define MCOL(A) ((A)->menu ? 1u : 0u)   /* clock tables: 0 V30, 1 286 (menu 2 uses the 286 column) */
static int is_prefix(unsigned op)
{
    return op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0xf2 || op == 0xf3;
}
static int is_string(unsigned op)
{
    return (op >= 0x6c && op <= 0x6f) || (op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf);
}
/* ip0/cx0: state before the step. ip1: IP after it. */
static unsigned clocks_of(Pc98Adapter *A, unsigned ip0, unsigned cx0, unsigned ip1, unsigned cl0)
{
    unsigned m = MCOL(A), i = 0, rep = 0, o32 = 0, op, modrm, idx, clk, n = A->nfetch > 8 ? 8 : A->nfetch;
    const unsigned m2 = A->menu == 2;
    while (i < n && (is_prefix(A->ib[i]) || (m2 && A->ib[i] >= 0x64 && A->ib[i] <= 0x67))) {
        if (A->ib[i] >= 0xf2) rep = A->ib[i];
        else if (A->ib[i] == 0x66) o32 = 1;
        i++;
    }
    if (i >= n) return 2;
    op = A->ib[i];
    modrm = (i + 1 < n) ? A->ib[i + 1] : 0;
    if (op == 0x0f) {
        unsigned second = modrm; modrm = (i + 2 < n) ? A->ib[i + 2] : 0;
        idx = (second << 4) | (((modrm >> 3) & 7u) << 1) | ((modrm >> 6) != 3);
        if (m2) {
            if (second == 0x20 || second == 0x22) return second == 0x20 ? 6u : 10u;   /* MOV r32,CR0 / MOV CR0,r32: 386 PRM clocks */
            clk = (o32 ? clock_386_0f66 : clock_386_0f)[idx];
            if (second >= 0x80 && second <= 0x8f && ip1 != (uint16_t)(ip0 + n)) clk += clock_386_jcc_taken;   /* Jcc near taken */
            return clk;
        }
        return clock_0f[m][idx];
    }
    idx = (op << 4) | (((modrm >> 3) & 7u) << 1) | ((modrm >> 6) != 3);
    if (m2 && (op == 0x8c || op == 0x8e) && (((modrm >> 3) & 7u) == 4u || ((modrm >> 3) & 7u) == 5u)) idx &= ~(7u << 1);   /* FS/GS cost like ES */
    clk = (m2 && o32 && op != 0xc8 && op != 0xc9 && op != 0xcf ? clock_386_main66[idx] : clock_main[m][idx]);   /* 66 C8/C9/CF: cost like the 16-bit forms */
    if (rep && is_string(op)) {
        const uint8_t *rz = m2 && o32 ? clock_386_rep66_zero : clock_rep_zero[m];
        const uint8_t *ri = m2 && o32 ? clock_386_rep66_iter : clock_rep_iter[m];
        if (!cx0) return rz[op];
        clk = ri[op];
        if (!(A->rep_valid && A->rep_ip == (uint16_t)ip0)) clk += rz[op];
        if (ip1 == (uint16_t)ip0) { A->rep_valid = 1; A->rep_ip = (uint16_t)ip0; } else A->rep_valid = 0;
        return clk;
    }
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0xe0 && op <= 0xe3)) {
        if (ip1 != (uint16_t)(ip0 + n)) clk += clock_taken[m][op];
    } else if (op == 0xc0 || op == 0xc1) {
        clk += clock_shift[m][op & 1u] * (A->ib[n - 1] & 31u);
    } else if (op == 0xd2 || op == 0xd3) {
        clk += clock_shift[m][2 + (op & 1u)] * (cl0 & 31u);
    } else if (op == 0xc8) {
        unsigned level = (i + 3 < n ? A->ib[i + 3] : 0) & 31u;
        if (level) clk = 15u + ((level - 1u) * 9u) / 2u;
    }
    return clk;
}

/* ---- stepping ---- */
#if !PC98_FULLCPU
extern const char *pc98_purge_reason;
#endif
static void stop_machine(Pc98Adapter *A, const char *why)
{
    const Pc98Cpu16State *c = &A->s.low;
    unsigned i;
    A->stopped = 1; PROFH(pc98_prof_stop());
    A->stop_pc = c->base[PC98_CS] + c->ip;
    for (i = 0; i < 8; i++) A->stop_bytes[i] = mem[(A->stop_pc + i) & 0x1fffffu];
#if !PC98_FULLCPU
    if (pc98_purge_reason) {
        ets_printf("NEWCPU STOP (%s; purged: %s): %04x:%04x bytes %02x %02x %02x %02x %02x %02x %02x %02x menu=%u\n", why, pc98_purge_reason,
                   c->seg[PC98_CS], c->ip, A->stop_bytes[0], A->stop_bytes[1], A->stop_bytes[2], A->stop_bytes[3],
                   A->stop_bytes[4], A->stop_bytes[5], A->stop_bytes[6], A->stop_bytes[7], A->menu);
        pc98_purge_reason = 0;
        return;
    }
#endif
    ets_printf("NEWCPU STOP (%s): %04x:%04x bytes %02x %02x %02x %02x %02x %02x %02x %02x menu=%u\n", why,
               c->seg[PC98_CS], c->ip, A->stop_bytes[0], A->stop_bytes[1], A->stop_bytes[2], A->stop_bytes[3],
               A->stop_bytes[4], A->stop_bytes[5], A->stop_bytes[6], A->stop_bytes[7], A->menu);
}
static const Pc98ExecuteBus *current_bus(Pc98Adapter *A)
{
    if (A->menu == 0) return &A->bus;
    if (!A->pbus_valid || A->pbus_msw != (A->mode.system.msw & 1u)) {
        A->pbus = pc98_cpu_mode_bus286(&A->mode, &A->s.low, &A->bus);
        A->pbus.task_committed = &A->switched;
        A->pbus_msw = A->mode.system.msw & 1u;
        A->pbus_valid = 1;
    }
    return &A->pbus;
}
/* Deliver a pending TF trap / fault. Returns 0 when delivery is impossible. */
static int deliver_fault(Pc98Adapter *A)
{
    Pc98Cpu16State *c = &A->s.low;
    const Pc98ExecuteBus *b = current_bus(A);
#ifdef PC98_ADAPT_TRACE_FAULTS
    ets_printf("cpu fault vec=%u err=%04x has=%u restart=%04x at %04x:%04x\n", c->fault.vector, c->fault.error, c->fault.has_error, c->fault.restart_ip, c->seg[PC98_CS], c->ip);
#endif
    if (A->menu == 0) {
        if (c->fault.vector != 1) return 0;
        c->fault = (Pc98CpuFault16){0};
        return pc98_cpu_interrupt16(c, b, 1, PC98_INT_SOFTWARE) == PC98_INT_DELIVERED;
    }
    return pc98_cpu_mode_deliver_pending286(&A->mode, c, b) == PC98_FAULT_DELIVERED;
}
enum Pc98StepResult pc98_adapt_step(unsigned *clocks)
{
    Pc98Adapter *A = &g_cpuad;
    Pc98Cpu16State *c = &A->s.low;
    const Pc98ExecuteBus *b = current_bus(A);
    unsigned ip0 = c->ip, cx0 = c->r[PC98_CX], cl0 = c->r[PC98_CX] & 255u;
    enum Pc98StepResult r;
    A->nfetch = 0; memset(A->ib, 0, sizeof(A->ib));
    if (A->mode.shutdown && A->menu >= 1) r = PC98_STEP_SHUTDOWN;
    else if (A->menu == 2) r = pc98_cpu_menu_step(2, &A->mode, &A->s, &A->bus);
    else r = pc98_cpu_step16(c, b);
    *clocks = 0;
    if (r == PC98_STEP_TRAP || r == PC98_STEP_FAULT) PROFH(pc98_prof_fault(c->fault.vector, r == PC98_STEP_TRAP));
    switch (r) {
    case PC98_STEP_OK:
        *clocks = clocks_of(A, ip0, cx0, c->ip, cl0);
        A->retired++;
        return r;
    case PC98_STEP_HALTED:
        return r;
    case PC98_STEP_TRAP:
    case PC98_STEP_FAULT:
        *clocks = r == PC98_STEP_TRAP ? clocks_of(A, ip0, cx0, c->ip, cl0) : 0;
        if (!deliver_fault(A)) { stop_machine(A, "undeliverable fault"); return PC98_STEP_UNSUPPORTED; }
        *clocks += clock_interrupt[MCOL(A)];
        A->retired++;
        return PC98_STEP_OK;
    case PC98_STEP_SHUTDOWN: stop_machine(A, "286 shutdown"); return r;
    case PC98_STEP_WAITING: stop_machine(A, "WAIT busy"); return r;
    default: stop_machine(A, "unsupported instruction"); return r;
    }
}

static int try_irq(Pc98Adapter *A, unsigned vector)
{
    Pc98Cpu16State *c = &A->s.low;
    const Pc98ExecuteBus *b = current_bus(A);
    enum Pc98InterruptResult r = A->menu == 0 ? pc98_cpu_interrupt16(c, b, vector, PC98_INT_IRQ)
                               : A->menu == 2 ? pc98_cpu_menu_interrupt(2, &A->mode, &A->s, &A->bus, vector, PC98_INT_IRQ)
                                              : pc98_cpu_mode_interrupt286(&A->mode, c, b, vector, PC98_INT_IRQ);
    if (r == PC98_INT_DELIVERED) { PROFH(pc98_prof_irq(vector)); g_cpu.slice_left -= clock_interrupt[MCOL(A)]; return 1; }
    if (r == PC98_INT_FAULT) { if (!deliver_fault(A)) stop_machine(A, "IRQ delivery fault"); return 1; }
    if (r == PC98_INT_UNSUPPORTED) { stop_machine(A, "IRQ delivery unsupported"); return 1; }
    return 0;
}
/* PIC has already accepted the request, so a blocked one (STI / MOV SS shadow)
 * must be latched and retried after the shadowed instruction. */
void pc98_adapt_interrupt(unsigned vector)
{
    Pc98Adapter *A = &g_cpuad;
    if (A->stopped) return;
    if (!try_irq(A, vector)) A->pending = vector | 0x100u;
}

#ifdef PC98_FAST16_STATS
/* Host profiling aid: instructions the fast path handed to the reference, by opcode. */
#include <stdio.h>
static unsigned long long f16_fb[256], f16_inel, f16_total_fb;
static void f16_count_fallback(const Pc98Adapter *A, int ineligible)
{
    unsigned i, op = 0;
    uint32_t pc = A->s.low.base[PC98_CS] + A->s.low.ip;
    if (ineligible) { f16_inel++; return; }
    for (i = 0; i < 8; i++) {
        op = mem[(pc + i) & 0xfffffu];
        if (!(op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0xf2 || op == 0xf3 || op == 0xf0 || (op >= 0x64 && op <= 0x67))) break;
    }
    f16_fb[op]++; f16_total_fb++;
}
extern unsigned long long pc98_fast16_slow_fetch[64][2];
__attribute__((destructor)) static void f16_report(void)
{
    unsigned i, j, best;
    unsigned long long shown = 0;
    if (!f16_total_fb && !f16_inel) return;
    fprintf(stderr, "FAST16 fallback: eligible-but-refused %llu, ineligible-state steps %llu\n", f16_total_fb, f16_inel);
    for (j = 0; j < 16; j++) {
        best = 0;
        for (i = 1; i < 256; i++) if (f16_fb[i] > f16_fb[best]) best = i;
        if (!f16_fb[best]) break;
        fprintf(stderr, "  op %02x: %llu\n", best, f16_fb[best]);
        shown += f16_fb[best]; f16_fb[best] = 0;
    }
    (void)shown;
    for (i = 0; i < 64; i++) if (pc98_fast16_slow_fetch[i][0] || pc98_fast16_slow_fetch[i][1]) fprintf(stderr, "  bus-fetched code page %02x (%05x): itf0=%llu itf1=%llu\n", i, i << 15, pc98_fast16_slow_fetch[i][0], pc98_fast16_slow_fetch[i][1]);
}
#endif

/* 7c-1b device profile, shown on the PCS line: fast16 batches, reference steps. */
unsigned g_cpuad_runs, g_cpuad_slow;
void pc98_adapt_prof(unsigned long long *ins, unsigned *runs, unsigned *slow)
{
    *ins = g_cpuad.retired; *runs = g_cpuad_runs; *slow = g_cpuad_slow;
}

#ifdef PC98_COST
extern uint32_t g_cost[16];
#define COST_RDI() ({ unsigned _v; __asm__ volatile("csrr %0, 0xb02" : "=r"(_v)); _v; })
#endif
void pc98_adapt_exec(void)
{
#ifdef PC98_COST
    unsigned ce0 = COST_RDI();
#endif
    Pc98Adapter *A = &g_cpuad;
    unsigned dma;
    if (!A->active || A->stopped) { if (g_cpu.slice_left > 0) g_cpu.slice_left = -1; return; }
    dma = g_dma_active != 0;
    A->rep_valid = 0;
#ifndef PC98_FAST16_DISABLE
    A->fast_ok = pc98_fast16_eligible(A);
#endif
    do {
        unsigned clk, irqop;
        enum Pc98StepResult r;
        if (A->pending) {
            if (try_irq(A, A->pending & 0xffu)) A->pending = 0;
#ifndef PC98_FAST16_DISABLE
            A->fast_ok = pc98_fast16_eligible(A);
#endif
        }
#ifdef PC98_PROF
        unsigned prof_path = A->fast_ok ? 1u : 2u;
        pc98_prof_pre(A);
#endif
#ifndef PC98_FAST16_DISABLE
        if (A->fast_ok) {
            /* DMA runs between instructions: one instruction (one REP iteration) per call */
            g_cpuad_runs++;
#ifdef PC98_COST
            unsigned cr0 = COST_RDI(); int cres = pc98_fast16_run(A, (dma || PROF_ONE) ? 1 : (int32_t)g_cpu.slice_left, ~0u, &clk); g_cost[9] += COST_RDI() - cr0; g_cost[10]++;
            if (cres) {
#else
            if (pc98_fast16_run(A, (dma || PROF_ONE) ? 1 : (int32_t)g_cpu.slice_left, ~0u, &clk)) {
#endif
                PROFH(pc98_prof_post(A, 0));
                /* fast16 already debited g_cpu.slice_left (A->slice_left_p) */
                if (A->irqend) { A->irqend = 0; IRQ_END_SLICE(); }
                if (A->s.low.flags & 0x100u) { A->fast_ok = 0; dma_cycle(); }
                else if (dma) { dma_cycle(); }
                continue;
            }
#ifdef PC98_FAST16_STATS
            f16_count_fallback(A, 0);
#endif
        }
#ifdef PC98_FAST16_STATS
        else f16_count_fallback(A, 1);
#endif
#endif
        g_cpuad_slow++;
        { uint32_t pc_ = (A->s.low.base[PC98_CS] + A->s.low.ip) & 0xfffffu; unsigned o_ = mem[pc_]; unsigned wasdi_ = !(A->s.low.flags & 0x200u);
          irqop = (o_ == 0xcf || o_ == 0x9d || (o_ == 0xfb && wasdi_)); }
        r = pc98_adapt_step(&clk);
        if (irqop && (A->s.low.flags & 0x200u) && pic_pending()) IRQ_END_SLICE();
        PROFH(pc98_prof_post(A, prof_path));
#ifndef PC98_FAST16_DISABLE
        A->fast_ok = pc98_fast16_eligible(A);
#endif
        if (r == PC98_STEP_HALTED || (r == PC98_STEP_OK && A->s.low.halted)) {
            g_cpu.slice_left -= clk;
            if (g_cpu.slice_left > 0) g_cpu.slice_left = -1;
            break;
        }
        if (r != PC98_STEP_OK) { if (g_cpu.slice_left > 0) g_cpu.slice_left = -1; break; }
        g_cpu.slice_left -= clk;
        if (dma || (A->s.low.flags & 0x100u)) { dma_cycle(); }
    } while (g_cpu.slice_left > 0);
    pc98_adapt_mirror();
#ifdef PC98_COST
    g_cost[8] += COST_RDI() - ce0;
#endif
}
