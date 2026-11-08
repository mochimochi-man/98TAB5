/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Real-mode fast path for the V30 / 80286 models, see fast16.h and DESIGN_CPU.md
 * "7c-1b". Every opcode below mirrors execute.c (step16) for the real-mode,
 * TF=0 case; anything the reference would fault on, reject, or that needs a
 * 64K-wrap decision is handed back (return 0) before any side effect.
 *
 * Conventions in pc98_fast16_step():
 *  - r[] / c fields are written only after the last bail-out point of the
 *    instruction; memory writes happen last, in the reference order.
 *  - fl / nip / clk are locals committed at "commit".
 *  - Linear addresses are base+offset without masking (the bus applies A20/HMA);
 *    RAM below 0xA4000 (read) / 0xA0000 (write) is accessed through A->ram. */
#include <stdint.h>
#include <string.h>
#include "fast16.h"
#include "purge.h"
#include "alu.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "clock_tables.inc"
/* stage 21: the mini loop in RISC-V assembly (fast16_mini_rv.S, its own loop: the C mini handlers it lacks run one
 * instruction at a time through f16x_cone); -DPC98_FAST16_CMINI keeps the C ones. F16_ASM: the C handlers the
 * assembly has are left out. */
#if defined(__riscv) && !defined(PC98_FAST16_CMINI)
#define F16_ASM 1
#else
#define F16_ASM 0
#endif

/* plain graphics VRAM (bus.c, video/vram.c): the mini loop's byte moves */
extern int g_gvram_plain;
unsigned gvram_plain_rd8(uint32_t a);
void gvram_plain_wr8(uint32_t a, unsigned v);
#pragma GCC diagnostic pop

#ifdef PC98_FAST16_WRITE_HOOK
void PC98_FAST16_WRITE_HOOK(uint32_t addr, uint8_t value);
#define F16_HOOK(a, v) PC98_FAST16_WRITE_HOOK((uint32_t)(a), (uint8_t)(v))
#else
#define F16_HOOK(a, v) ((void)0)
#endif

/* ITF ROM image offset inside mem[] (the ITF bank is kept 100000h above the BIOS bank, bus.c rom_r8) */
#define F16_ITF_STEP 0x100000u
enum { F_CF = 1, F_PF = 4, F_AF = 16, F_ZF = 64, F_SF = 128, F_TF = 256, F_IF = 512, F_DF = 1024, F_OF = 2048 };
enum { SG_ES = 0, SG_CS = 1, SG_SS = 2, SG_DS = 3 };
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

#define B2(n) n, (n) ^ 4, (n) ^ 4, n
#define B4(n) B2(n), B2((n) ^ 4), B2((n) ^ 4), B2(n)
#define B6(n) B4(n), B4((n) ^ 4), B4((n) ^ 4), B4(n)
/* PF (bit 2) when the low byte has even parity */
static const uint8_t par_tab[256] = { B6(4), B6(0), B6(0), B6(4) };

/* ---- eligibility ---- */
int pc98_fast16_eligible(const Pc98Adapter *A)
{
    const Pc98Cpu16State *c = &A->s.low;
    unsigned s;
    if (A->menu > 2 || !A->active || A->stopped || !A->ram) return 0;
    if (c->halted || c->fault.valid || (c->flags & F_TF)) return 0;
    for (s = 0; s < 4; s++) if (c->base[s] > 0xffff0u) return 0;
    if (A->menu == 0) return c->model == PC98_CPU_V30;
    if (c->model != PC98_CPU_286 || (A->mode.system.msw & 1u) || A->mode.shutdown) return 0;
    if (A->menu == 2 && PC98_V86(&A->s)) return 0;   /* V86 */
    for (s = 0; s < 4; s++) {
        const Pc98Descriptor286 *d = &A->mode.segments.cache[s];
        if (!A->mode.segments.valid[s] || d->limit != 0xffffu || d->base != c->base[s]) return 0;
        /* fast16 assumes plain real caches: readable code CS, writable data ES/SS/DS
         * (a LOADALL read-only or execute-only cache keeps its checks in the reference) */
        if ((d->access & 0x9au) != (s == SG_CS ? 0x9au : 0x92u)) return 0;
    }
    return A->mode.interrupts.idt.base == 0 && A->mode.interrupts.idt.limit >= 0x3ffu;
}

/* ---- ALU (bit-exact with pc98_cpu_alu / pc98_cpu_shift) ---- */
static inline unsigned f_add16(unsigned a, unsigned b, unsigned cin, unsigned *fl)
{
    unsigned w = a + b + cin, r = w & 0xffffu;
    unsigned f = ((w >> 16) & 1u) | ((a ^ b ^ r) & F_AF) | par_tab[r & 255u] | (r ? 0u : F_ZF) | ((r >> 8) & F_SF) |
                 (((~(a ^ b) & (a ^ r)) >> 4) & F_OF);
    *fl = (*fl & ~(unsigned)F_ARITH) | f;
    return r;
}
static inline unsigned f_sub16(unsigned a, unsigned b, unsigned cin, unsigned *fl)
{
    unsigned w = a - b - cin, r = w & 0xffffu;
    unsigned f = ((w >> 16) & 1u) | ((a ^ b ^ r) & F_AF) | par_tab[r & 255u] | (r ? 0u : F_ZF) | ((r >> 8) & F_SF) |
                 ((((a ^ b) & (a ^ r)) >> 4) & F_OF);
    *fl = (*fl & ~(unsigned)F_ARITH) | f;
    return r;
}
static inline unsigned f_add8(unsigned a, unsigned b, unsigned cin, unsigned *fl)
{
    unsigned w = a + b + cin, r = w & 0xffu;
    unsigned f = ((w >> 8) & 1u) | ((a ^ b ^ r) & F_AF) | par_tab[r] | (r ? 0u : F_ZF) | (r & F_SF) |
                 (((~(a ^ b) & (a ^ r)) << 4) & F_OF);
    *fl = (*fl & ~(unsigned)F_ARITH) | f;
    return r;
}
static inline unsigned f_sub8(unsigned a, unsigned b, unsigned cin, unsigned *fl)
{
    unsigned w = a - b - cin, r = w & 0xffu;
    unsigned f = ((w >> 8) & 1u) | ((a ^ b ^ r) & F_AF) | par_tab[r] | (r ? 0u : F_ZF) | (r & F_SF) |
                 ((((a ^ b) & (a ^ r)) << 4) & F_OF);
    *fl = (*fl & ~(unsigned)F_ARITH) | f;
    return r;
}
/* OR/AND/XOR: CF=OF=0, AF kept */
static inline unsigned f_log16(unsigned r, unsigned *fl)
{
    *fl = (*fl & ~(unsigned)(F_CF | F_PF | F_ZF | F_SF | F_OF)) | par_tab[r & 255u] | (r ? 0u : F_ZF) | ((r >> 8) & F_SF);
    return r;
}
static inline unsigned f_log8(unsigned r, unsigned *fl)
{
    *fl = (*fl & ~(unsigned)(F_CF | F_PF | F_ZF | F_SF | F_OF)) | par_tab[r] | (r ? 0u : F_ZF) | (r & F_SF);
    return r;
}
static __attribute__((noinline)) unsigned alu16(unsigned op, unsigned a, unsigned b, unsigned *fl)
{
    switch (op) {
    case 0: return f_add16(a, b, 0, fl);
    case 1: return f_log16(a | b, fl);
    case 2: return f_add16(a, b, *fl & F_CF, fl);
    case 3: return f_sub16(a, b, *fl & F_CF, fl);
    case 4: return f_log16(a & b, fl);
    case 6: return f_log16(a ^ b, fl);
    default: return f_sub16(a, b, 0, fl);   /* 5 SUB, 7 CMP */
    }
}
static __attribute__((noinline)) unsigned alu8(unsigned op, unsigned a, unsigned b, unsigned *fl)
{
    switch (op) {
    case 0: return f_add8(a, b, 0, fl);
    case 1: return f_log8(a | b, fl);
    case 2: return f_add8(a, b, *fl & F_CF, fl);
    case 3: return f_sub8(a, b, *fl & F_CF, fl);
    case 4: return f_log8(a & b, fl);
    case 6: return f_log8(a ^ b, fl);
    default: return f_sub8(a, b, 0, fl);
    }
}
/* INC/DEC keep CF */
static inline unsigned f_inc16(unsigned a, unsigned *fl)
{
    unsigned r = (a + 1u) & 0xffffu;
    *fl = (*fl & ~(unsigned)(F_ARITH & ~F_CF)) | ((a ^ 1u ^ r) & F_AF) | par_tab[r & 255u] | (r ? 0u : F_ZF) |
          ((r >> 8) & F_SF) | (a == 0x7fffu ? F_OF : 0u);
    return r;
}
static inline unsigned f_dec16(unsigned a, unsigned *fl)
{
    unsigned r = (a - 1u) & 0xffffu;
    *fl = (*fl & ~(unsigned)(F_ARITH & ~F_CF)) | ((a ^ 1u ^ r) & F_AF) | par_tab[r & 255u] | (r ? 0u : F_ZF) |
          ((r >> 8) & F_SF) | (a == 0x8000u ? F_OF : 0u);
    return r;
}
static inline unsigned f_inc8(unsigned a, unsigned *fl)
{
    unsigned r = (a + 1u) & 0xffu;
    *fl = (*fl & ~(unsigned)(F_ARITH & ~F_CF)) | ((a ^ 1u ^ r) & F_AF) | par_tab[r] | (r ? 0u : F_ZF) | (r & F_SF) |
          (a == 0x7fu ? F_OF : 0u);
    return r;
}
static inline unsigned f_dec8(unsigned a, unsigned *fl)
{
    unsigned r = (a - 1u) & 0xffu;
    *fl = (*fl & ~(unsigned)(F_ARITH & ~F_CF)) | ((a ^ 1u ^ r) & F_AF) | par_tab[r] | (r ? 0u : F_ZF) | (r & F_SF) |
          (a == 0x80u ? F_OF : 0u);
    return r;
}
/* Group-2 shifts/rotates, cnt >= 1 (op 0..5,7). Same results as the reference's
 * iterate-count-times loop, in closed form. */
static inline __attribute__((always_inline)) unsigned f_shiftN(unsigned op, unsigned B, unsigned v, unsigned cnt, unsigned *fl);
static __attribute__((noinline)) unsigned f_shift(unsigned op, unsigned B, unsigned v, unsigned cnt, unsigned *fl)
{
    return f_shiftN(op, B, v, cnt, fl);
}
/* the same, flags by value: (new FLAGS << 16) | result. The mini handlers use this form: a handler that passes the
 * address of fl cannot leave with a sibling call, and the assembly handlers (fast16_mini_rv.S) need every C handler
 * to leave with one (a normal call would run the next handler with the caller's s registers). */
static __attribute__((noinline)) uint32_t f_shift_v(unsigned op, unsigned B, unsigned v, unsigned cnt, unsigned fl)
{
    const unsigned r = f_shiftN(op, B, v, cnt, &fl);
    return (fl << 16) | r;
}
static inline __attribute__((always_inline)) unsigned f_shiftN(unsigned op, unsigned B, unsigned v, unsigned cnt, unsigned *fl)
{
    const unsigned M = (1u << B) - 1u, S = 1u << (B - 1u);
    unsigned r, cf, f, mask = F_CF;
    switch (op) {
    case 0: { unsigned k = cnt % B; r = k ? ((v << k) | (v >> (B - k))) & M : v; cf = r & 1u; break; }
    case 1: { unsigned k = cnt % B; r = k ? ((v >> k) | (v << (B - k))) & M : v; cf = (r & S) != 0; break; }
    case 2: case 3: {
        unsigned k = cnt % (B + 1u), W = B + 1u, MW = (1u << W) - 1u, x = v | ((*fl & F_CF) << B);
        if (k) x = op == 2 ? ((x << k) | (x >> (W - k))) & MW : ((x >> k) | (x << (W - k))) & MW;
        r = x & M; cf = x >> B; break;
    }
    case 4:
        if (cnt <= B) { r = (v << cnt) & M; cf = (v >> (B - cnt)) & 1u; } else { r = 0; cf = 0; }
        break;
    case 5:
        if (cnt <= B) { r = v >> cnt; cf = (v >> (cnt - 1u)) & 1u; } else { r = 0; cf = 0; }
        break;
    default: /* 7 SAR */
        if (cnt < B) {
            unsigned sv = (v & S) ? (v | ~M) : v;
            r = (unsigned)((int)sv >> cnt) & M; cf = (v >> (cnt - 1u)) & 1u;
        } else { r = (v & S) ? M : 0u; cf = (v & S) != 0; }
        break;
    }
    f = cf;
    if (op >= 4) {
        mask |= F_ZF | F_SF | F_PF;
        f |= (r ? 0u : F_ZF) | (B == 8 ? (r & F_SF) : ((r >> 8) & F_SF)) | par_tab[r & 255u];
    }
    if (cnt == 1) {
        unsigned of;
        mask |= F_OF;
        if (op == 0 || op == 2 || op == 4) of = ((r & S) != 0) ^ cf;
        else if (op == 1 || op == 3) of = ((r & S) != 0) ^ ((r & (S >> 1)) != 0);
        else of = op == 5 && (v & S);
        if (of) f |= F_OF;
    }
    *fl = (*fl & ~mask) | (f & mask);
    return r;
}

/* Count-1 shift/rotate (D0/D1), all of op 0..5,7 inline; same results as f_shift(op, B, v, 1, fl). B is a constant at the call sites. */
static inline __attribute__((always_inline)) unsigned f_shift1(unsigned op, unsigned B, unsigned v, unsigned *fl)
{
    const unsigned M = (1u << B) - 1u, S = 1u << (B - 1u), cin = *fl & F_CF;
    unsigned r, cf, of, f, mask = F_CF | F_OF;
    switch (op) {
    case 0: r = ((v << 1) | (v >> (B - 1u))) & M; cf = r & 1u; of = ((r & S) != 0) ^ cf; break;
    case 1: r = ((v >> 1) | (v << (B - 1u))) & M; cf = (r & S) != 0; of = cf ^ ((r & (S >> 1)) != 0); break;
    case 2: r = ((v << 1) | cin) & M; cf = (v >> (B - 1u)) & 1u; of = ((r & S) != 0) ^ cf; break;
    case 3: r = (v >> 1) | (cin << (B - 1u)); cf = v & 1u; of = ((r & S) != 0) ^ ((r & (S >> 1)) != 0); break;
    case 4: r = (v << 1) & M; cf = (v >> (B - 1u)) & 1u; of = ((r & S) != 0) ^ cf; break;
    case 5: r = v >> 1; cf = v & 1u; of = (v & S) != 0; break;
    default: r = (v >> 1) | (v & S); cf = v & 1u; of = 0; break;   /* 7 SAR */
    }
    f = cf | (of ? F_OF : 0u);
    if (op >= 4) {
        mask |= F_ZF | F_SF | F_PF;
        f |= (r ? 0u : F_ZF) | (B == 8 ? (r & F_SF) : ((r >> 8) & F_SF)) | par_tab[r & 255u];
    }
    *fl = (*fl & ~mask) | (f & mask);
    return r;
}
static inline int cond_true(unsigned code, unsigned fl)
{
    unsigned res;
    switch (code >> 1) {
    case 0: res = (fl & F_OF) != 0; break;
    case 1: res = (fl & F_CF) != 0; break;
    case 2: res = (fl & F_ZF) != 0; break;
    case 3: res = (fl & (F_CF | F_ZF)) != 0; break;
    case 4: res = (fl & F_SF) != 0; break;
    case 5: res = (fl & F_PF) != 0; break;
    case 6: res = ((fl & F_SF) != 0) ^ ((fl & F_OF) != 0); break;
    default: res = (((fl & F_SF) != 0) ^ ((fl & F_OF) != 0)) | ((fl & F_ZF) != 0); break;
    }
    return (int)(res ^ (code & 1u));
}

/* ModRM effective address; *np advances past displacement. */
static inline unsigned ea_calc(const uint16_t *r, unsigned mod, unsigned rm, const uint8_t *p, unsigned *np, unsigned *dseg)
{
    static const uint8_t ea1[8] = { 3, 3, 5, 5, 6, 7, 5, 3 };   /* first register: BX BX BP BP SI DI BP BX */
    unsigned off, n = *np, d = SG_DS;
    if (__builtin_expect(rm == 6u && mod == 0, 0)) { off = (unsigned)p[n] | ((unsigned)p[n + 1] << 8); n += 2; }
    else {
        off = r[ea1[rm]];
        if (rm < 4u) off += r[6u + (rm & 1u)];
        if ((0x4cu >> rm) & 1u) d = SG_SS;   /* BP+SI, BP+DI, BP */
    }
    if (mod == 1) off += (unsigned)(int)(int8_t)p[n++];
    else if (mod == 2) { off += (unsigned)p[n] | ((unsigned)p[n + 1] << 8); n += 2; }
    *np = n; *dseg = d;
    return off & 0xffffu;
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
_Static_assert(__builtin_offsetof(Pc98Adapter, s.low.r) + 8 < 256, "A->r8o holds byte offsets");
/* byte register i: constant index folds, a variable one reads the per-adapter offset table (A->r8o) */
#define R8PTR(i) (__builtin_constant_p(i) ? (uint8_t *)r + ((((i) & 3u) << 1) | ((i) >> 2)) : (uint8_t *)A + A->r8o[(i) & 7u])
#define GET8(i) ((unsigned)*R8PTR(i))
#define SET8(i, v) (*R8PTR(i) = (uint8_t)(v))
#else
#define GET8(i) ((unsigned)(r[(i) & 3u] >> (((i) & 4u) << 1)) & 255u)
#define SET8(i, v) do { unsigned _s = ((i) & 4u) << 1; r[(i) & 3u] = (uint16_t)((r[(i) & 3u] & ~(255u << _s)) | (((unsigned)(v) & 255u) << _s)); } while (0)
#endif

/* ---- slow (device / bus) accesses: out of line so the hot loop stays small ----
 * Devices may look at IF, AX and the CPU clock (g_cpu.slice_left), and may retarget it. The
 * first bus access of an instruction publishes FLAGS and the exact remaining clock
 * (remv = clocks left before this instruction, already offset); later ones do not
 * (the device may have changed g_cpu.slice_left in between). */
#define EXT 2u        /* a bus access happened in this instruction */
#define HARD 1u       /* the batch must end after this instruction */
#define IOX 4u        /* a port access happened (A20, pending IRQ may have changed) */
/* STI / IRET / POPF with IF=1 and a deliverable IRQ: end the batch and ask the adapter to end the slice so pic_deliver() runs now */
#define IRQ_END_CHK() do { if ((fl & F_IF) && A->irqprobe && A->irqprobe()) { stop |= HARD; A->irqend = 1; } } while (0)
static inline void slow_sync(Pc98Adapter *A, unsigned fl, int32_t remv, unsigned first)
{
    if (first) {
        A->s.low.flags = (uint16_t)fl;
        if (A->slice_left_p) *A->slice_left_p = remv;
    }
}
static __attribute__((noinline)) unsigned slow_rb(Pc98Adapter *A, uint32_t a, unsigned fl, int32_t remv, unsigned first)
{
    slow_sync(A, fl, remv, first);
    return A->devrb ? A->devrb(a) : A->bus.read8(A->bus.context, a);
}
static __attribute__((noinline)) unsigned slow_rw(Pc98Adapter *A, uint32_t a, unsigned fl, int32_t remv, unsigned first)
{
    slow_sync(A, fl, remv, first);
    return A->devrw ? A->devrw(a) : A->bus.read16(A->bus.context, a);
}
static __attribute__((noinline)) void slow_wb(Pc98Adapter *A, uint32_t a, unsigned v, unsigned fl, int32_t remv, unsigned first)
{
    slow_sync(A, fl, remv, first);
    if (A->devwb) A->devwb(a, v); else A->bus.write8(A->bus.context, a, (uint8_t)v);
}
static __attribute__((noinline)) void slow_ww(Pc98Adapter *A, uint32_t a, unsigned v, unsigned fl, int32_t remv, unsigned first)
{
    slow_sync(A, fl, remv, first);
    if (A->devww) A->devww(a, v); else A->bus.write16(A->bus.context, a, (uint16_t)v);
}
static __attribute__((noinline)) unsigned slow_in(Pc98Adapter *A, unsigned port, unsigned wide, unsigned fl, int32_t remv, unsigned first)
{
    slow_sync(A, fl, remv, first);
    return wide ? A->bus.in16(A->bus.context, (uint16_t)port) : A->bus.in8(A->bus.context, (uint16_t)port);
}
static __attribute__((noinline)) void slow_out(Pc98Adapter *A, unsigned port, unsigned wide, unsigned v, unsigned fl, int32_t remv, unsigned first)
{
    slow_sync(A, fl, remv, first);
    if (wide) A->bus.out16(A->bus.context, (uint16_t)port, (uint16_t)v);
    else A->bus.out8(A->bus.context, (uint16_t)port, (uint8_t)v);
}
#if F16_ASM   /* fast16_mini_rv.S port I/O: the first bus access of the instruction */
unsigned f16x_in(Pc98Adapter *A, unsigned port, unsigned wide, unsigned fl, int32_t remv);
void f16x_out(Pc98Adapter *A, unsigned port, unsigned wide, unsigned v, unsigned fl, int32_t remv);
unsigned f16x_in(Pc98Adapter *A, unsigned port, unsigned wide, unsigned fl, int32_t remv) { return slow_in(A, port, wide, fl, remv, 1u); }
void f16x_out(Pc98Adapter *A, unsigned port, unsigned wide, unsigned v, unsigned fl, int32_t remv) { slow_out(A, port, wide, v, fl, remv, 1u); }
#endif
/* instruction bytes through the bus (non-RAM, non-ROM code) */
static __attribute__((noinline)) void slow_fetch(Pc98Adapter *A, uint32_t lin, uint8_t *win, unsigned fl, int32_t remv)
{
    unsigned i;
    uint8_t (*fetch)(void *, uint32_t) = A->bus.fetch8 ? A->bus.fetch8 : A->bus.read8;
    void *fctx = A->bus.fetch8 ? A->bus.fetch_context : A->bus.context;
    slow_sync(A, fl, remv, 1);
    for (i = 0; i < 10; i++) win[i] = fetch(fctx, lin + i);
}

/* segment register load; the 286 also refreshes the real-mode hidden cache */
static __attribute__((noinline)) void load_seg(Pc98Adapter *A, unsigned sg, unsigned val)
{
    Pc98Cpu16State *const c = &A->s.low;
    val &= 0xffffu;
    c->seg[sg] = (uint16_t)val; c->base[sg] = (uint32_t)val << 4;
    if (A->menu != 0) {
        Pc98Descriptor286 *d = &A->mode.segments.cache[sg];
        d->base = c->base[sg]; d->limit = 0xffffu; d->access = sg == SG_CS ? 0x9bu : 0x93u;
        A->mode.segments.valid[sg] = 1;
    }
}

/* Opcode classes, built once: bit0 has ModR/M, bit1 prefix */
/* Opcodes whose case in fast16_body.inc starts with DEC_MODRM(): ALU r/m forms, 80-85, 88-8B, 8D */
#define F16_SELFDEC(op) 0
static uint8_t optab[256], optab2[256];   /* optab2: menu 2 (286+386 extensions), bit 4 = always refused */
static uint8_t ckn[2][256], ckm2[2][512];   /* clock by opcode / by opcode+memory-operand (reg-independent opcodes) */
static unsigned optab_ready;
static void optab_init(void)
{
    unsigned op;
    for (op = 0; op < 256; op++) {
        unsigned f = 0;
        if (op < 0x40u) f = (op & 7u) < 4u;
        else if ((op >= 0x80u && op <= 0x8fu) || op == 0x69u || op == 0x6bu) f = 1;
        else if (op == 0xc0u || op == 0xc1u || (op >= 0xc4u && op <= 0xc7u) || (op >= 0xd0u && op <= 0xd3u)) f = 1;
        else if (op == 0xf6u || op == 0xf7u || op == 0xfeu || op == 0xffu) f = 1;
        if (op == 0x26u || op == 0x2eu || op == 0x36u || op == 0x3eu || op == 0xf2u || op == 0xf3u) f = 2;
        if (f == 1u && F16_SELFDEC(op)) f = 0;   /* the case body decodes ModR/M itself (per-opcode branch sites) */
        optab[op] = (uint8_t)f;
        /* menu 2 runs these through the extension path: 86/87 9C/9D CF C0/C1 D0-D3 (0F and 64-67 are never handled here) */
        optab2[op] = (uint8_t)(f | ((op == 0x9du) ? 4u : 0u));
    }
    for (op = 0; op < 2; op++) {
        unsigned o;
        for (o = 0; o < 256; o++) {
            ckn[op][o] = clock_main[op][(o << 4) | 1u];
            ckm2[op][(o << 1) | 0u] = clock_main[op][(o << 4) | 0u];
            ckm2[op][(o << 1) | 1u] = clock_main[op][(o << 4) | 1u];
        }
    }
    optab_ready = 1;
}

/* MUL / IMUL / DIV / IDIV (F6 F7 /4../7) on AX / DX:AX; 0 = divide error (nothing changed). Same results as pc98_cpu_muldiv. */
static inline int f_muldiv(unsigned reg, unsigned bits, uint16_t *r, unsigned v, unsigned *flp)
{
    unsigned fl = *flp;
    if (bits == 16) {
        const unsigned ax = r[0], dx = r[2];
        if (reg <= 5) {
            unsigned ovf;
            uint32_t pr;
            if (reg == 4) { pr = (uint32_t)ax * v; ovf = (pr >> 16) != 0; }
            else { int32_t sp32 = (int32_t)(int16_t)ax * (int32_t)(int16_t)v; pr = (uint32_t)sp32; ovf = sp32 != (int32_t)(int16_t)sp32; }
            r[0] = (uint16_t)pr; r[2] = (uint16_t)(pr >> 16);
            fl = (fl & ~(unsigned)(F_CF | F_OF)) | (ovf ? (F_CF | F_OF) : 0u);
        } else if (reg == 6) {
            uint32_t dd = ((uint32_t)dx << 16) | ax, q;
            if (!v) return 0;
            q = dd / v;
            if (q > 0xffffu) return 0;
            r[0] = (uint16_t)q; r[2] = (uint16_t)(dd % v);
        } else {
            int32_t dd = (int32_t)(((uint32_t)dx << 16) | ax), dv = (int16_t)v, q;
            if (!dv || (dv == -1 && dd == INT32_MIN)) return 0;
            q = dd / dv;
            if (q < -32768 || q > 32767) return 0;
            r[0] = (uint16_t)q; r[2] = (uint16_t)(dd % dv);
        }
    } else {
        const unsigned ax = r[0];
        if (reg <= 5) {
            unsigned ovf;
            uint32_t pr;
            if (reg == 4) { pr = (ax & 255u) * v; ovf = (pr >> 8) != 0; }
            else { int32_t s32 = (int32_t)(int8_t)ax * (int32_t)(int8_t)v; pr = (uint32_t)s32; ovf = s32 != (int32_t)(int8_t)s32; }
            r[0] = (uint16_t)pr;
            fl = (fl & ~(unsigned)(F_CF | F_OF)) | (ovf ? (F_CF | F_OF) : 0u);
        } else if (reg == 6) {
            unsigned q;
            if (!v) return 0;
            q = ax / v;
            if (q > 0xffu) return 0;
            r[0] = (uint16_t)((q & 255u) | ((ax % v) << 8));
        } else {
            int32_t dd = (int16_t)ax, dv = (int8_t)v, q;
            if (!dv) return 0;
            q = dd / dv;
            if (q < -128 || q > 127) return 0;
            r[0] = (uint16_t)(((unsigned)q & 255u) | (((unsigned)(dd % dv) & 255u) << 8));
        }
    }
    *flp = fl;
    return 1;
}

/* Per-instruction state shared with the out-of-line opcode handlers. */
typedef struct {
    Pc98Adapter *A;
    const uint8_t *p;
    uint8_t *hma;
    uint32_t lin, cs_lin;
    int32_t rem, off;
    unsigned fl, ip0, n, ovr, rep, op, xmod, xreg, xrm, ea, padd, stop;
    unsigned nip, clk, completed, iters, refresh;
} F16Ctx;

/* Memory accessors. They expect these locals: A ram hma fl rem off stop sy. RAM / HMA stay inline. */
#define SLOWARGS fl, (int32_t)(rem + off), sy
#define RDB(a) (__builtin_expect((a) < 0xa4000u, 1) ? (unsigned)ram[(a)] : ((a) - 0x100000u < 0x10000u && hma) ? (unsigned)hma[(a) - 0x100000u] : \
        (sy = ~stop & EXT, stop |= EXT, slow_rb(A, (a), SLOWARGS)))
#define RDW(a) (__builtin_expect((a) < 0xa3fffu, 1) ? (unsigned)ram[(a)] | ((unsigned)ram[(a) + 1] << 8) : \
        ((a) - 0x100000u < 0xffffu && hma) ? (unsigned)hma[(a) - 0x100000u] | ((unsigned)hma[(a) - 0x100000u + 1] << 8) : \
        (sy = ~stop & EXT, stop |= EXT, slow_rw(A, (a), SLOWARGS)))
#define WRB(a, v) do { uint32_t _a = (a); unsigned _v = (v) & 255u; \
        if (__builtin_expect(_a < 0xa0000u, 1)) { F16_HOOK(_a, _v); ram[_a] = (uint8_t)_v; } \
        else if (_a - 0x100000u < 0x10000u && hma) { F16_HOOK(_a, _v); hma[_a - 0x100000u] = (uint8_t)_v; } \
        else { sy = ~stop & EXT; stop |= EXT; slow_wb(A, _a, _v, SLOWARGS); } } while (0)
#define WRW(a, v) do { uint32_t _a = (a); unsigned _v = (v) & 0xffffu; \
        if (__builtin_expect(_a < 0x9ffffu, 1)) { F16_HOOK(_a, _v & 255u); F16_HOOK(_a + 1, _v >> 8); ram[_a] = (uint8_t)_v; ram[_a + 1] = (uint8_t)(_v >> 8); } \
        else if (_a - 0x100000u < 0xffffu && hma) { uint8_t *_h = hma + (_a - 0x100000u); F16_HOOK(_a, _v & 255u); F16_HOOK(_a + 1, _v >> 8); _h[0] = (uint8_t)_v; _h[1] = (uint8_t)(_v >> 8); } \
        else { sy = ~stop & EXT; stop |= EXT; slow_ww(A, _a, _v, SLOWARGS); } } while (0)
#define IMM8() ((unsigned)p[n++])
#define IMM16() (n += 2, (unsigned)p[n - 2] | ((unsigned)p[n - 1] << 8))
#define SX8(v) ((unsigned)(int)(int8_t)(v) & 0xffffu)
#define CLKMF() ((unsigned)clkm[(op << 4) | (reg << 1) | (mod != 3)])
#define CLKM() ((unsigned)ckm_[(op << 1) | (mod != 3)])
#define CLKN() ((unsigned)ckn_[op])
#define WCHK() do { if (mod != 3 && ea == 0xffffu) BAIL(); } while (0)
#define RM8() (mod == 3 ? GET8(rm) : RDB(lin))
#define RM16() (mod == 3 ? (unsigned)r[rm] : RDW(lin))
#define PUT_RM8(v) do { if (mod == 3) SET8(rm, (v)); else WRB(lin, (v)); } while (0)
#define PUT_RM16(v) do { if (mod == 3) r[rm] = (uint16_t)(v); else WRW(lin, (v)); } while (0)
#define SSB (c->base[SG_SS])
#define PUSHW(v) do { sp = (sp - 2u) & 0xffffu; WRW(SSB + sp, (v)); } while (0)
#define LOADSEG(sg, val) load_seg(A, (sg), (val))
#define LOADSEG_I(sg, val) do { unsigned _v = (val) & 0xffffu; c->seg[sg] = (uint16_t)_v; c->base[sg] = (uint32_t)_v << 4; \
        if (!v30) { Pc98Descriptor286 *_d = &A->mode.segments.cache[sg]; _d->base = c->base[sg]; _d->limit = 0xffffu; \
            _d->access = (sg) == SG_CS ? 0x9bu : 0x93u; A->mode.segments.valid[sg] = 1; } } while (0)

#define STORE1() do { if (!dev) { F16_HOOK(da, v0); ram[da] = (uint8_t)v0; if (w == 2u) { F16_HOOK(da + 1u, v1); ram[da + 1u] = (uint8_t)v1; } } \
        else { A->s.low.flags = (uint16_t)fl; if (A->slice_left_p) *A->slice_left_p = remv; \
            if (w == 2u) { if (A->devww) A->devww(da, v0 | (v1 << 8)); else A->bus.write16(A->bus.context, da, (uint16_t)(v0 | (v1 << 8))); } \
            else { if (A->devwb) A->devwb(da, v0); else A->bus.write8(A->bus.context, da, (uint8_t)v0); } } } while (0)
#define HMA_REFRESH() (hma = (A->hma && A->addr_mask_p && (*A->addr_mask_p & 0x100000u)) ? A->hma : NULL)
/* One non-REP MOVS / STOS / LODS with the source in plain RAM. Returns 1 when done inside RAM, 2 when the
 * destination was device memory (the clock was published, caller must pick up g_cpu.slice_left again), 0 when
 * nothing was done (generic handler). */
static __attribute__((noinline)) int str1(Pc98Adapter *A, unsigned op, unsigned ovr, unsigned fl, int32_t remv)
{
    Pc98Cpu16State *const c = &A->s.low;
    uint16_t *const r = c->r;
    uint8_t *const ram = A->ram;
    const unsigned w = 1u + (op & 1u);
    const unsigned sdir = (fl & F_DF) ? 0x10000u - w : w;
    uint32_t sa = 0, da = 0;
    unsigned v0, v1, dev = 0;
    if (op != 0xaa && op != 0xab) {   /* uses DS:SI */
        if (w == 2u && r[6] == 0xffffu) return 0;
        sa = c->base[ovr != 4 ? ovr : SG_DS] + r[6];
        if (sa + w > 0xa4000u) return 0;
    }
    if (op <= 0xab) {                 /* uses ES:DI */
        if (w == 2u && r[7] == 0xffffu) return 0;
        da = c->base[SG_ES] + r[7];
        dev = da + w > 0xa0000u;
    }
    if (op <= 0xa5) {                 /* MOVS */
        v0 = ram[sa]; v1 = w == 2u ? ram[sa + 1u] : 0u;
        STORE1();
        r[6] = (uint16_t)(r[6] + sdir); r[7] = (uint16_t)(r[7] + sdir);
    } else if (op <= 0xab) {          /* STOS */
        v0 = r[0] & 255u; v1 = (unsigned)r[0] >> 8;
        STORE1();
        r[7] = (uint16_t)(r[7] + sdir);
    } else {                          /* LODS */
        if (w == 2u) r[0] = (uint16_t)(ram[sa] | (ram[sa + 1u] << 8)); else SET8(0, ram[sa]);
        r[6] = (uint16_t)(r[6] + sdir);
    }
    return dev ? 2 : 1;
}


static void optab_init(void);
#include "fast16_mini.inc"
#if F16_ASM
#include "fast16_rv.h"
#define F16A_CHK(v, f) _Static_assert((v) == __builtin_offsetof(Pc98Adapter, f), "fast16_rv.h: " #f);
F16A_CHK(F16A_R, s.low.r) F16A_CHK(F16A_SEG, s.low.seg) F16A_CHK(F16A_FLAGS, s.low.flags) F16A_CHK(F16A_BASE, s.low.base)
F16A_CHK(F16A_IRQINH, s.low.irq_inhibit) F16A_CHK(F16A_CACHE, mode.segments.cache) F16A_CHK(F16A_VALID, mode.segments.valid)
F16A_CHK(F16A_MENU, menu) F16A_CHK(F16A_ROM, rom) F16A_CHK(F16A_ITF, itf) F16A_CHK(F16A_SLICEP, slice_left_p)
F16A_CHK(F16A_IRQPROBE, irqprobe) F16A_CHK(F16A_IRQEND, irqend) F16A_CHK(F16A_MCKN, mck_n) F16A_CHK(F16A_MCKM, mck_m)
F16A_CHK(F16A_MCKMAIN, mck_main) F16A_CHK(F16A_MCKT, mck_t) F16A_CHK(F16A_MOPCB, mo_pcb) F16A_CHK(F16A_MCKSHIFT, mck_shift)
F16A_CHK(F16A_MOREM, mo_rem) F16A_CHK(F16A_MOOFF, mo_off) F16A_CHK(F16A_MOFL, mo_fl) F16A_CHK(F16A_MONRET, mo_nret)
F16A_CHK(F16A_MOIP, mo_ip) F16A_CHK(F16A_MOEV, mo_ev) F16A_CHK(F16A_MOOV, mo_ov) F16A_CHK(F16A_MOROM, mo_rom)
_Static_assert(sizeof(Pc98Descriptor286) == 8 && __builtin_offsetof(Pc98Descriptor286, limit) == 4 && __builtin_offsetof(Pc98Descriptor286, access) == 6, "descriptor layout");
#undef F16A_CHK
/* fast16_mini_rv.S: per index i, the handler of opcode i, the byte register offsets of the r/m and reg field of
 * ModR/M i, PF of byte i, the clocks of opcode i (register form / no ModR/M, memory form) and its handler after a
 * segment prefix (internal RAM: read every instruction) */
typedef struct { const void *h; uint8_t rmb, regb, pf, cr, cm, pad[3]; const void *hov; } F16xRec;
_Static_assert(sizeof(F16xRec) == 16 && __builtin_offsetof(F16xRec, rmb) == 4 && __builtin_offsetof(F16xRec, cr) == 7 &&
               __builtin_offsetof(F16xRec, hov) == 12, "F16xRec layout");
F16xRec f16x_rec[256] __attribute__((section(".dram1.f16x_rec"), aligned(16)));
extern const void *const f16x_hdef[256], *const f16x_hov[256];
extern const char f16x_h_cstep[], f16x_h_exit[], f16x_h_bailov[];
void f16x_start(Pc98Adapter *A, const uint8_t *pc, int32_t rem, unsigned fl, const uint8_t *lim, uint8_t *ram);
#define M_START(A_, P_, R_, F_, L_, N_, T_, M_) f16x_start((A_), (P_), (R_), (F_), (L_), (M_))
#define M_TABOK(op) (f16x_rec[(op)].h != (const void *)f16x_h_exit)
void f16x_cone(Pc98Adapter *A, const uint8_t *pc, int32_t rem, unsigned fl, unsigned nret);
/* one instruction of an opcode the assembly does not have: the C mini handler, its batch ended right after it */
void f16x_cone(Pc98Adapter *A, const uint8_t *pc, int32_t rem, unsigned fl, unsigned nret)
{
    (void)m_start(A, pc, rem, fl, pc + 1, nret, (const void *const *)mtab, A->ram);
}
static void f16x_init(void)
{
    unsigned i;
    for (i = 0; i < 256; i++) {
        F16xRec *const d = &f16x_rec[i];
        const unsigned rm = i & 7u, reg = (i >> 3) & 7u;
        d->h = f16x_hdef[i] ? f16x_hdef[i] : mtab[i] != m_exit ? (const void *)f16x_h_cstep : (const void *)f16x_h_exit;
        d->hov = f16x_hov[i] ? f16x_hov[i] : (const void *)f16x_h_bailov;
        d->rmb = (uint8_t)(((rm & 3u) << 1) | (rm >> 2));
        d->regb = (uint8_t)(((reg & 3u) << 1) | (reg >> 2));
        d->pf = par_tab[i];
    }
}
/* the clocks of the CPU model (mini_models): ModR/M opcodes take mck_m, the others mck_n */
static void f16x_clocks(const Pc98Adapter *A)
{
    unsigned op;
    for (op = 0; op < 256; op++) {
        const unsigned modrm = (op < 0x40u && (op & 7u) < 4u) || (op >= 0x80u && op <= 0x8fu) || op == 0xc0u || op == 0xc1u ||
                               op == 0xc6u || op == 0xc7u || (op >= 0xd0u && op <= 0xd3u) || op == 0xf6u || op == 0xf7u || op >= 0xfeu;
        f16x_rec[op].cr = modrm ? A->mck_m[op * 2u] : A->mck_n[op];
        f16x_rec[op].cm = A->mck_m[op * 2u + 1u];
    }
}
#else
#define M_START m_start
#endif
#ifndef M_TABOK
#define M_TABOK(op) (mtab[(op)] != m_exit)   /* the opcode at the batch start has a mini handler */
#endif
static void mini_models(Pc98Adapter *A, unsigned mi)
{
    unsigned o;
    for (o = 0; o < 256; o++) { A->mck_n[o] = ckn[mi][o]; A->mck_m[o * 2] = ckm2[mi][o * 2]; A->mck_m[o * 2 + 1] = ckm2[mi][o * 2 + 1]; }
    A->mck_main = clock_main[mi]; A->mck_t = clock_taken[mi]; A->mck_shift = clock_shift[mi]; A->mck_mi = mi + 1u;
#if F16_ASM
    f16x_clocks(A);
#endif
}

#define F16_MODEL 1
#define F16_SFX m286
#include "fast16_body.inc"
#undef F16_MODEL
#undef F16_SFX

int pc98_fast16_run(Pc98Adapter *A, int32_t budget, unsigned max_insn, unsigned *clocks)
{
    /* one instantiation serves the V30, the 286 and the 286+386 model: v30 / ext are run-time flags (IRAM budget) */
    return pc98_fast16_run_m286(A, budget, max_insn, clocks);
}


int pc98_fast16_step(Pc98Adapter *A, int32_t budget, unsigned *clocks)
{
    return pc98_fast16_run(A, budget, 1u, clocks);
}
