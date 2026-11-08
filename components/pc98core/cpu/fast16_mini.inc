/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * fast16 "mini loop" (stage 14): the hottest real-mode opcodes as tail-call threaded handlers.
 *
 * Every handler is a separate function with the same signature; the machine state that changes every
 * instruction travels in the argument registers (A, pc, rem, fl, lim, nret, tab, ram) and the next handler
 * is entered with a sibling call, so there is no dispatch loop, no stack traffic and no per-instruction
 * decode of prefix / ModR/M classes.
 *
 * Contract (anything else is handed back to the generic loop in fast16_body.inc, which then executes the
 * instruction exactly as before):
 *  - entered with stop == 0 and no interrupt-inhibit shadow
 *  - a handler that cannot finish the instruction returns through m_exit() before any side effect
 *  - memory: RAM below A0000h (writes) / A4000h (reads) only, no 64K wrap, no HMA, no device memory
 *  - an instruction that needs the generic loop to react (STI shadow, TF, deliverable IRQ) retires and leaves with A->mo_ev set
 *  - the batch ends (m_exit) when the clock budget is used up or pc reaches lim (IP wrap / end of RAM) */

typedef unsigned (*mh_t)(Pc98Adapter *, const uint8_t *, int32_t, unsigned, const uint8_t *, unsigned, const void *const *, uint8_t *);
#define MARGS Pc98Adapter *A, const uint8_t *pc, int32_t rem, unsigned fl, const uint8_t *lim, unsigned nret, const void *const *tab, uint8_t *ram
#define MP A, pc, rem, fl, lim, nret, tab, ram
#define MH(name) static unsigned name(MARGS)
enum { MEV_HARD = 1, MEV_INH = 2, MEV_WIN = 4 };   /* A->mo_ev: end the batch after this instruction / an interrupt-inhibit shadow was created / the code window may have moved */

static __attribute__((noinline)) unsigned m_exit(MARGS)
{
    (void)lim; (void)tab; (void)ram;
    A->mo_ip = (unsigned)(pc - A->mo_pcb); A->mo_rem = rem; A->mo_fl = fl; A->mo_nret = nret;
    return 0;
}
#define MBAIL() return m_exit(MP)
#define MDISPATCH() return ((mh_t)tab[*pc])(MP)
/* retire the instruction: debit its clocks, advance to the next one, continue unless the batch is over */
#define MGO(NEWPC, CLK) do { rem -= (int32_t)(CLK); pc = (NEWPC); nret++; \
        if (__builtin_expect(rem <= 0, 0) || __builtin_expect(pc >= lim, 0)) return m_exit(MP); \
        MDISPATCH(); } while (0)
#define MNEXT(NB, CLK) MGO(pc + (NB), CLK)
/* retire and hand back to the generic loop, which acts on EV */
#define MEXIT_EV(NEWPC, CLK, EV) do { rem -= (int32_t)(CLK); pc = (NEWPC); nret++; A->mo_ev |= (EV); return m_exit(MP); } while (0)
#define MCKN(op) ((unsigned)A->mck_n[(op)])
#define MCKM(op, mem) ((unsigned)A->mck_m[((op) << 1) | (mem)])
#define MIP() ((unsigned)(pc - A->mo_pcb))
/* code window of a CS base: RAM below A4000h, or the BIOS ROM area (E8000h-FFFFFh) while the ITF bank is not selected (the generic loop's fetch rule); 0 = none */
#define M_CSWIN(b_) ((b_) <= 0xa3ff6u ? 0xa3ff7u - (b_) : ((b_) >= 0xe8000u && (b_) <= 0xffff6u && A->rom == (const uint8_t *)ram && !*A->itf) ? 0xffff7u - (b_) : 0u)
/* CS changed: new code base and the matching window */
#define MSETCS(BASE, IPV) do { const uint32_t b_ = (BASE); const uint8_t *const pcb_ = ram + b_; const uint32_t w_ = M_CSWIN(b_), sp_ = w_ < 0xfff0u ? w_ : 0xfff0u;         A->mo_pcb = pcb_; A->mo_rom = b_ >= 0xe8000u; lim = pcb_ + sp_; pc = pcb_ + (IPV); } while (0)
#define M_LOADSEG(sg, val) do { Pc98Cpu16State *const c_ = &A->s.low; const unsigned v_ = (val) & 0xffffu; \
        c_->seg[(sg)] = (uint16_t)v_; c_->base[(sg)] = (uint32_t)v_ << 4; \
        if (A->menu != 0) { Pc98Descriptor286 *const d_ = &A->mode.segments.cache[(sg)]; d_->base = c_->base[(sg)]; d_->limit = 0xffffu; \
            d_->access = (sg) == SG_CS ? 0x9bu : 0x93u; A->mode.segments.valid[(sg)] = 1; } } while (0)
#define M_IRQCHK(EV) do { if ((fl & F_IF) && A->irqprobe && A->irqprobe()) { (EV) |= MEV_HARD; A->irqend = 1; } } while (0)

typedef struct { uint16_t o1, o2, bo; uint8_t dlen, pad; uint32_t mk8, mk16; } EaEnt;
static EaEnt g_eat[192];

/* ModR/M effective address from the table: no data-dependent branches */
#define MEA(m) const EaEnt *const e_ = &g_eat[(m)]; \
    { const unsigned d16_ = (unsigned)pc[2] | ((unsigned)pc[3] << 8); \
      ea = ((unsigned)*(const uint16_t *)((const char *)A + e_->o1) + (unsigned)*(const uint16_t *)((const char *)A + e_->o2) + \
            ((d16_ & e_->mk16) | ((unsigned)(int)(int8_t)pc[2] & e_->mk8))) & 0xffffu; \
      lin = *(const uint32_t *)((const char *)A + e_->bo) + ea; nb = 2u + e_->dlen; }
#define MMODRM() const unsigned m = pc[1], mem = m < 0xc0u, reg = (m >> 3) & 7u, rm = m & 7u; \
    unsigned nb = 2, ea = 0; uint32_t lin = 0; (void)reg; (void)rm; (void)ea; (void)lin; \
    if (mem) { MEA(m); }

#define M_RD16(a) ((unsigned)ram[(a)] | ((unsigned)ram[(a) + 1] << 8))
#define M_WR8(a, v) do { const uint32_t _a = (a); const unsigned _v = (v) & 255u; F16_HOOK(_a, _v); ram[_a] = (uint8_t)_v; } while (0)
#define M_WR16(a, v) do { const uint32_t _a = (a); const unsigned _v = (v) & 0xffffu; F16_HOOK(_a, _v & 255u); F16_HOOK(_a + 1, _v >> 8); \
        ram[_a] = (uint8_t)_v; ram[_a + 1] = (uint8_t)(_v >> 8); } while (0)
/* bounds of plain RAM for a memory operand (checked before anything changes) */
#define M_CHK_R16() do { if (mem && (ea == 0xffffu || lin >= 0xa3fffu)) MBAIL(); } while (0)
#define M_CHK_RW16() do { if (mem && (ea == 0xffffu || lin >= 0x9ffffu)) MBAIL(); } while (0)
#define M_CHK_R8() do { if (mem && lin >= 0xa4000u) MBAIL(); } while (0)
/* Plain graphics VRAM (A8000h-BFFFFh, no GRCG / EGC) byte moves stay in the loop: the bus handlers are called
 * directly with the clock handed over and taken back, as for port I/O, instead of leaving the loop. */
#define M_GVRAM(a) ((uint32_t)((a) - 0xa8000u) < 0x18000u && g_gvram_plain >= 0 && A->slice_left_p)
#define M_GVRD8(a) ({ unsigned v_; *A->slice_left_p = rem + A->mo_off; \
    v_ = gvram_plain_rd8(a); rem = *A->slice_left_p - A->mo_off; v_; })
#define M_GVWR8(a, v) do { *A->slice_left_p = rem + A->mo_off; \
    gvram_plain_wr8((a), (v)); rem = *A->slice_left_p - A->mo_off; } while (0)
#define M_CHK_RW8() do { if (mem && lin >= 0xa0000u) MBAIL(); } while (0)

#define M_ALU16(aop, a, b, res) switch (aop) { \
        case 0: res = f_add16(a, b, 0, &fl); break; case 1: res = f_log16((a) | (b), &fl); break; \
        case 2: res = f_add16(a, b, fl & F_CF, &fl); break; case 3: res = f_sub16(a, b, fl & F_CF, &fl); break; \
        case 4: res = f_log16((a) & (b), &fl); break; case 6: res = f_log16((a) ^ (b), &fl); break; \
        default: res = f_sub16(a, b, 0, &fl); break; }
#define M_ALU8(aop, a, b, res) switch (aop) { \
        case 0: res = f_add8(a, b, 0, &fl); break; case 1: res = f_log8((a) | (b), &fl); break; \
        case 2: res = f_add8(a, b, fl & F_CF, &fl); break; case 3: res = f_sub8(a, b, fl & F_CF, &fl); break; \
        case 4: res = f_log8((a) & (b), &fl); break; case 6: res = f_log8((a) ^ (b), &fl); break; \
        default: res = f_sub8(a, b, 0, &fl); break; }

#if !F16_ASM   /* fast16_mini_rv.S has these */
/* ---- ALU acc,imm ---- */
#define M_ALU_ACC(OPC, AOP, FORM) MH(m_##OPC) { \
    uint16_t *const r = A->s.low.r; unsigned res, a, b; \
    if ((FORM) == 4) { b = pc[1]; a = r[0] & 255u; M_ALU8(AOP, a, b, res); if ((AOP) != 7) SET8(0, res); MNEXT(2, MCKN(OPC)); } \
    b = (unsigned)pc[1] | ((unsigned)pc[2] << 8); a = r[0]; M_ALU16(AOP, a, b, res); if ((AOP) != 7) r[0] = (uint16_t)res; MNEXT(3, MCKN(OPC)); }
M_ALU_ACC(0x04, 0, 4) M_ALU_ACC(0x05, 0, 5) M_ALU_ACC(0x0c, 1, 4) M_ALU_ACC(0x0d, 1, 5) M_ALU_ACC(0x14, 2, 4) M_ALU_ACC(0x15, 2, 5)
M_ALU_ACC(0x1c, 3, 4) M_ALU_ACC(0x1d, 3, 5) M_ALU_ACC(0x24, 4, 4) M_ALU_ACC(0x25, 4, 5) M_ALU_ACC(0x2c, 5, 4) M_ALU_ACC(0x2d, 5, 5)
M_ALU_ACC(0x34, 6, 4) M_ALU_ACC(0x35, 6, 5) M_ALU_ACC(0x3c, 7, 4) M_ALU_ACC(0x3d, 7, 5)

#endif
#if !F16_ASM
MH(m_a8) { uint16_t *const r = A->s.low.r; (void)f_log8((r[0] & 255u) & pc[1], &fl); MNEXT(2, MCKN(0xa8)); }
MH(m_a9) { uint16_t *const r = A->s.low.r; (void)f_log16(r[0] & ((unsigned)pc[1] | ((unsigned)pc[2] << 8)), &fl); MNEXT(3, MCKN(0xa9)); }
#endif
/* one MOVS / STOS without REP or a segment prefix: str1 (fast16.c), as the body does it, without leaving the loop */
MH(m_str1)
{
    const unsigned op = pc[0];
    int rv;
    if (!A->slice_left_p) MBAIL();
    rv = str1(A, op, 4u, fl, rem + A->mo_off);
    if (!rv) MBAIL();
    if (rv == 2) rem = *A->slice_left_p - A->mo_off;
    MNEXT(1, MCKN(op));
}

/* ---- INC / DEC reg16, PUSH / POP reg16 ---- */
#if !F16_ASM
MH(m_inc) { uint16_t *const r = A->s.low.r; const unsigned op = pc[0]; r[op & 7u] = (uint16_t)f_inc16(r[op & 7u], &fl); MNEXT(1, MCKN(op)); }
MH(m_dec) { uint16_t *const r = A->s.low.r; const unsigned op = pc[0]; r[op & 7u] = (uint16_t)f_dec16(r[op & 7u], &fl); MNEXT(1, MCKN(op)); }
MH(m_push)
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], sp = r[4];
    const unsigned nsp = (sp - 2u) & 0xffffu; const uint32_t a = A->s.low.base[SG_SS] + nsp;
    if (sp == 1u || a >= 0x9ffffu) MBAIL();
    M_WR16(a, r[op & 7u]); r[4] = (uint16_t)nsp;
    MNEXT(1, MCKN(op));
}
MH(m_pop)
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], sp = r[4];
    const uint32_t a = A->s.low.base[SG_SS] + sp;
    if (sp == 0xffffu || a >= 0xa3fffu) MBAIL();
    r[4] = (uint16_t)(sp + 2u); r[op & 7u] = (uint16_t)M_RD16(a);
    MNEXT(1, MCKN(op));
}
#endif
#if !F16_ASM
MH(m_pushseg)   /* 06 0E 16 1E */
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], sp = r[4];
    const unsigned nsp = (sp - 2u) & 0xffffu; const uint32_t a = A->s.low.base[SG_SS] + nsp;
    if (sp == 1u || a >= 0x9ffffu) MBAIL();
    M_WR16(a, A->s.low.seg[op >> 3]); r[4] = (uint16_t)nsp;
    MNEXT(1, MCKN(op));
}
MH(m_popseg)    /* 07 1F (POP SS has an interrupt shadow: generic loop) */
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], sp = r[4];
    const uint32_t a = A->s.low.base[SG_SS] + sp;
    unsigned v;
    if (sp == 0xffffu || a >= 0xa3fffu) MBAIL();
    v = M_RD16(a); r[4] = (uint16_t)(sp + 2u);
    M_LOADSEG(op >> 3, v);
    MNEXT(1, MCKN(op));
}
MH(m_pushf)
{
    uint16_t *const r = A->s.low.r; const unsigned sp = r[4], v30 = A->menu == 0;
    const unsigned v = v30 ? ((fl & 0xfd5u) | 2u | 0xf000u) : ((fl & 0x7fd5u) | 2u), nsp = (sp - 2u) & 0xffffu;
    const uint32_t a = A->s.low.base[SG_SS] + nsp;
    if (sp == 1u || a >= 0x9ffffu) MBAIL();
    M_WR16(a, v); r[4] = (uint16_t)nsp;
    MNEXT(1, MCKN(0x9c));
}
#endif
MH(m_popf)
{
    uint16_t *const r = A->s.low.r; const unsigned sp = r[4], v30 = A->menu == 0;
    const uint32_t a = A->s.low.base[SG_SS] + sp;
    unsigned v, ev = 0;
    if (sp == 0xffffu || a >= 0xa3fffu || A->menu == 2) MBAIL();   /* POPF in the 386 model is the reference's */
    v = M_RD16(a); r[4] = (uint16_t)(sp + 2u);
    fl = v30 ? ((v & 0xfd5u) | 2u) : ((v & 0xfd5u) | 2u | (fl & 0x7000u));
    if (fl & F_TF) ev |= MEV_HARD;
    M_IRQCHK(ev);
    if (ev) MEXIT_EV(pc + 1, MCKN(0x9d), ev);
    MNEXT(1, MCKN(0x9d));
}
MH(m_sti)
{
    unsigned ev = MEV_INH;
    const unsigned wasdi = !(fl & F_IF);
    fl |= F_IF; A->s.low.irq_inhibit = 1;   /* the generic loop sets 2 and decrements at the end of the instruction */
    if (wasdi) M_IRQCHK(ev);
    MEXIT_EV(pc + 1, MCKN(0xfb), ev);
}

/* ---- MOV ---- */
#if !F16_ASM
MH(m_8d) { uint16_t *const r = A->s.low.r; MMODRM(); if (!mem) MBAIL(); r[reg] = (uint16_t)ea; MNEXT(nb, MCKM(0x8d, 1)); }
MH(m_movi8) { uint16_t *const r = A->s.low.r; const unsigned op = pc[0]; SET8(op & 7u, pc[1]); MNEXT(2, MCKN(op)); }
MH(m_movi16) { uint16_t *const r = A->s.low.r; const unsigned op = pc[0]; r[op & 7u] = (uint16_t)((unsigned)pc[1] | ((unsigned)pc[2] << 8)); MNEXT(3, MCKN(op)); }
MH(m_xchg) { uint16_t *const r = A->s.low.r; const unsigned op = pc[0]; const uint16_t t = r[op & 7u]; r[op & 7u] = r[0]; r[0] = t; MNEXT(1, MCKN(op)); }
MH(m_cbw) { uint16_t *const r = A->s.low.r; r[0] = (uint16_t)((r[0] & 255u) | ((r[0] & 128u) ? 0xff00u : 0u)); MNEXT(1, MCKN(0x98)); }
MH(m_cwd) { uint16_t *const r = A->s.low.r; r[2] = (r[0] & 0x8000u) ? 0xffffu : 0u; MNEXT(1, MCKN(0x99)); }
MH(m_sahf) { uint16_t *const r = A->s.low.r; fl = (fl & ~0xd5u) | ((r[0] >> 8) & 0xd5u); MNEXT(1, MCKN(0x9e)); }
MH(m_lahf) { uint16_t *const r = A->s.low.r; SET8(4, (fl & 0xd5u) | 2u); MNEXT(1, MCKN(0x9f)); }
MH(m_cmc) { fl ^= F_CF; MNEXT(1, MCKN(0xf5)); }
MH(m_clc) { fl &= ~(unsigned)F_CF; MNEXT(1, MCKN(0xf8)); }
MH(m_stc) { fl |= F_CF; MNEXT(1, MCKN(0xf9)); }
MH(m_cli) { fl &= ~(unsigned)F_IF; MNEXT(1, MCKN(0xfa)); }
MH(m_cld) { fl &= ~(unsigned)F_DF; MNEXT(1, MCKN(0xfc)); }
MH(m_std) { fl |= F_DF; MNEXT(1, MCKN(0xfd)); }
#endif

/* ---- control transfer ---- */
#if !F16_ASM
#define M_JCC(opc, cnd) MH(m_##opc) { \
    const int d8 = (int)(int8_t)pc[1]; unsigned clk = MCKN(opc); \
    if (cnd) { if (d8) clk += A->mck_t[opc]; MGO(A->mo_pcb + ((MIP() + 2u + (unsigned)d8) & 0xffffu), clk); } \
    MNEXT(2, clk); }
#define M_SLT() ((((fl) >> 7) ^ ((fl) >> 11)) & 1u)
M_JCC(0x70, fl & F_OF)                  M_JCC(0x71, !(fl & F_OF))
M_JCC(0x72, fl & F_CF)                  M_JCC(0x73, !(fl & F_CF))
M_JCC(0x74, fl & F_ZF)                  M_JCC(0x75, !(fl & F_ZF))
M_JCC(0x76, fl & (F_CF | F_ZF))         M_JCC(0x77, !(fl & (F_CF | F_ZF)))
M_JCC(0x78, fl & F_SF)                  M_JCC(0x79, !(fl & F_SF))
M_JCC(0x7a, fl & F_PF)                  M_JCC(0x7b, !(fl & F_PF))
M_JCC(0x7c, M_SLT())                    M_JCC(0x7d, !M_SLT())
M_JCC(0x7e, M_SLT() || (fl & F_ZF))     M_JCC(0x7f, !(M_SLT() || (fl & F_ZF)))
#endif
#if !F16_ASM
MH(m_loop)   /* E0 E1 E2 E3 */
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0];
    const unsigned d = (unsigned)(int)(int8_t)pc[1] & 0xffffu;
    unsigned take, clk = MCKN(op);
    if (op == 0xe3u) take = r[1] == 0;
    else {
        r[1] = (uint16_t)(r[1] - 1u);
        take = r[1] != 0;
        if (op == 0xe0u) take = take && !(fl & F_ZF);
        if (op == 0xe1u) take = take && (fl & F_ZF);
    }
    if (take) {
        if (d) clk += A->mck_t[op];
        MGO(A->mo_pcb + ((MIP() + 2u + d) & 0xffffu), clk);
    }
    MNEXT(2, clk);
}
MH(m_eb) { const int d8 = (int)(int8_t)pc[1]; MGO(A->mo_pcb + ((MIP() + 2u + (unsigned)d8) & 0xffffu), MCKN(0xeb)); }
MH(m_e9) { const unsigned d = (unsigned)pc[1] | ((unsigned)pc[2] << 8); MGO(A->mo_pcb + ((MIP() + 3u + d) & 0xffffu), MCKN(0xe9)); }
MH(m_e8)
{
    uint16_t *const r = A->s.low.r; const unsigned sp = r[4], d = (unsigned)pc[1] | ((unsigned)pc[2] << 8), ret = (MIP() + 3u) & 0xffffu;
    const unsigned nsp = (sp - 2u) & 0xffffu; const uint32_t a = A->s.low.base[SG_SS] + nsp;
    if (sp == 1u || a >= 0x9ffffu) MBAIL();
    M_WR16(a, ret); r[4] = (uint16_t)nsp;
    MGO(A->mo_pcb + ((ret + d) & 0xffffu), MCKN(0xe8));
}
MH(m_c3)
{
    uint16_t *const r = A->s.low.r; const unsigned sp = r[4];
    const uint32_t a = A->s.low.base[SG_SS] + sp;
    unsigned v;
    if (sp == 0xffffu || a >= 0xa3fffu) MBAIL();
    v = M_RD16(a); r[4] = (uint16_t)(sp + 2u);
    MGO(A->mo_pcb + v, MCKN(0xc3));
}
#endif
MH(m_cf)   /* IRET */
{
    uint16_t *const r = A->s.low.r; const unsigned sp = r[4], ext = A->menu == 2;
    const uint32_t a = A->s.low.base[SG_SS] + sp;
    unsigned tgt, sel, f, ev = 0;
    if (sp > 0xfffau || a >= 0xa3ff9u) MBAIL();
    tgt = M_RD16(a); sel = M_RD16(a + 2u); f = M_RD16(a + 4u);
    r[4] = (uint16_t)(sp + 6u);
    M_LOADSEG(SG_CS, sel);
    fl = (f & (ext ? 0x7fd5u : 0xfd5u)) | 2u;
    if (ext) { A->s.low.cpl = 0; A->s.low.cpl_valid = 1; }
    A->s.low.nmi_active = 0;
    M_IRQCHK(ev);
    if (fl & F_TF) ev |= MEV_HARD;
    rem -= (int32_t)MCKN(0xcf); nret++;
    MSETCS(A->s.low.base[SG_CS], tgt & 0xffffu);
    A->mo_ev |= ev;
    if (ev || rem <= 0 || pc >= lim) return m_exit(MP);
    MDISPATCH();
}
MH(m_int)   /* CC CD (INTO is rare: generic loop) */
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], sp = r[4], v30 = A->menu == 0;
    const unsigned vec = op == 0xcdu ? pc[1] : 3u, ilen = op == 0xcdu ? 2u : 1u;
    const unsigned fv = v30 ? ((fl & 0xfd5u) | 2u | 0xf000u) : ((fl & 0x7fd5u) | 2u);
    const unsigned s1 = (sp - 2u) & 0xffffu, s2 = (sp - 4u) & 0xffffu, s3 = (sp - 6u) & 0xffffu;
    const uint32_t ssb = A->s.low.base[SG_SS];
    unsigned tgt, sel;
    if ((sp != 0 && sp < 6u) || s1 == 0xffffu || s2 == 0xffffu || s3 == 0xffffu) MBAIL();
    if (ssb + s1 >= 0x9ffffu || ssb + s2 >= 0x9ffffu || ssb + s3 >= 0x9ffffu) MBAIL();
    M_WR16(ssb + s1, fv); M_WR16(ssb + s2, A->s.low.seg[SG_CS]); M_WR16(ssb + s3, (MIP() + ilen) & 0xffffu);
    r[4] = (uint16_t)s3;
    fl &= ~(unsigned)(F_IF | F_TF);
    tgt = M_RD16(vec * 4u); sel = M_RD16(vec * 4u + 2u);
    M_LOADSEG(SG_CS, sel);
    A->s.low.irq_inhibit = A->s.low.nmi_inhibit = 0;
    if (!v30) A->s.low.debug_inhibit = 0;
    rem -= (int32_t)MCKN(op); nret++;
    MSETCS(A->s.low.base[SG_CS], tgt & 0xffffu);
    if (rem <= 0 || pc >= lim) return m_exit(MP);
    MDISPATCH();
}

/* ---- port I/O (devices may retarget the clock: the live g_cpu.slice_left is picked up afterwards) ---- */
#if !F16_ASM   /* fast16_mini_rv.S has the port I/O, the prefixes and the override forms */
MH(m_io)   /* E4 E5 E6 E7 EC ED EE EF */
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], wide = op & 1u, nb = op < 0xecu ? 2u : 1u;
    const unsigned port = op < 0xecu ? pc[1] : r[2];
    if (!A->slice_left_p || (port - 0xf0u) < 8u) MBAIL();   /* F0..F7: CPU reset / A20 */
    if (op & 2u) slow_out(A, port, wide, r[0], fl, rem + A->mo_off, 1u);
    else { const unsigned v = slow_in(A, port, wide, fl, rem + A->mo_off, 1u); if (wide) r[0] = (uint16_t)v; else SET8(0, v); }
    rem = *A->slice_left_p - A->mo_off;
    if (A->mo_rom) MEXIT_EV(pc + nb, MCKN(op), MEV_WIN);   /* a port write may switch the ITF bank: look at the code window again */
    MNEXT(nb, MCKN(op));
}

static mh_t mtab_ov[256];
static __attribute__((noinline)) unsigned mo_exit(MARGS) { return m_exit(A, pc - 1, rem, fl, lim, nret, tab, ram); }
/* segment override prefix: continue in the override table with A->mo_ov naming the segment base */
MH(m_pfx)
{
    A->mo_ov = (unsigned)__builtin_offsetof(Pc98Adapter, s.low.base) + 4u * (((unsigned)pc[0] >> 3) & 3u);
    pc++;
    return ((mh_t)mtab_ov[*pc])(MP);
}
#endif
static mh_t mtab[256];
static unsigned mtab_ready;

#define MINI_OV 0
#include "fast16_mini_mem.inc"
#undef MINI_OV
#if !F16_ASM
#define MINI_OV 1
#include "fast16_mini_mem.inc"
#undef MINI_OV
#endif

static __attribute__((noinline)) unsigned m_start(MARGS) { MDISPATCH(); }
#if F16_ASM
static void f16x_init(void);
#endif
static void mini_init(void)
{
    unsigned i, m;
#if F16_ASM
    for (i = 0; i < 256; i++) mtab[i] = m_exit;
    m_register(mtab);
#else
    for (i = 0; i < 256; i++) { mtab[i] = m_exit; mtab_ov[i] = mo_exit; }
    m_register(mtab); mo_register(mtab_ov);
#endif
#define MSET(op, h) (mtab[op] = (h))
#if !F16_ASM
    MSET(0x04, m_0x04); MSET(0x05, m_0x05); MSET(0x0c, m_0x0c); MSET(0x0d, m_0x0d); MSET(0x14, m_0x14); MSET(0x15, m_0x15);
    MSET(0x1c, m_0x1c); MSET(0x1d, m_0x1d); MSET(0x24, m_0x24); MSET(0x25, m_0x25); MSET(0x2c, m_0x2c); MSET(0x2d, m_0x2d);
    MSET(0x34, m_0x34); MSET(0x35, m_0x35); MSET(0x3c, m_0x3c); MSET(0x3d, m_0x3d);
    for (i = 0x40; i < 0x48; i++) { MSET(i, m_inc); MSET(i + 8, m_dec); }
    for (i = 0x50; i < 0x58; i++) { if (i != 0x54) MSET(i, m_push); if (i + 8 != 0x5c) MSET(i + 8, m_pop); }   /* PUSH SP / POP SP differ on the V30 */
#endif
#if !F16_ASM
    MSET(0x06, m_pushseg); MSET(0x0e, m_pushseg); MSET(0x16, m_pushseg); MSET(0x1e, m_pushseg);
    MSET(0x07, m_popseg); MSET(0x1f, m_popseg);
#endif
#if !F16_ASM
    MSET(0x26, m_pfx); MSET(0x2e, m_pfx); MSET(0x36, m_pfx); MSET(0x3e, m_pfx);
#endif
#if !F16_ASM
    MSET(0x70, m_0x70); MSET(0x71, m_0x71); MSET(0x72, m_0x72); MSET(0x73, m_0x73); MSET(0x74, m_0x74); MSET(0x75, m_0x75); MSET(0x76, m_0x76); MSET(0x77, m_0x77);
    MSET(0x78, m_0x78); MSET(0x79, m_0x79); MSET(0x7a, m_0x7a); MSET(0x7b, m_0x7b); MSET(0x7c, m_0x7c); MSET(0x7d, m_0x7d); MSET(0x7e, m_0x7e); MSET(0x7f, m_0x7f);
#endif
#if !F16_ASM
    MSET(0xa8, m_a8); MSET(0xa9, m_a9);
#endif
    MSET(0xa4, m_str1); MSET(0xa5, m_str1); MSET(0xaa, m_str1); MSET(0xab, m_str1);   /* LODS stays out: with STOS also here, a V30 program goes wrong after its notice screen - cause not found */
#if !F16_ASM
    MSET(0x8d, m_8d);
    for (i = 0x90; i < 0x98; i++) MSET(i, m_xchg);
    MSET(0x98, m_cbw); MSET(0x99, m_cwd); MSET(0x9c, m_pushf); MSET(0x9e, m_sahf); MSET(0x9f, m_lahf);
    for (i = 0xb0; i < 0xb8; i++) { MSET(i, m_movi8); MSET(i + 8, m_movi16); }
    MSET(0xc3, m_c3);
    for (i = 0xe0; i < 0xe4; i++) MSET(i, m_loop);
#endif
    MSET(0x9d, m_popf);
    MSET(0xcc, m_int); MSET(0xcd, m_int); MSET(0xcf, m_cf);
#if !F16_ASM
    MSET(0xe4, m_io); MSET(0xe5, m_io); MSET(0xe6, m_io); MSET(0xe7, m_io); MSET(0xec, m_io); MSET(0xed, m_io); MSET(0xee, m_io); MSET(0xef, m_io);
#endif
#if !F16_ASM
    MSET(0xe8, m_e8); MSET(0xe9, m_e9); MSET(0xeb, m_eb);
    MSET(0xf5, m_cmc); MSET(0xf8, m_clc); MSET(0xf9, m_stc); MSET(0xfa, m_cli); MSET(0xfc, m_cld); MSET(0xfd, m_std);
#endif
    MSET(0xfb, m_sti);
#undef MSET
    /* effective-address table: modrm 00..BF */
    for (m = 0; m < 192; m++) {
        static const uint8_t ea1[8] = { 3, 3, 5, 5, 6, 7, 5, 3 };   /* first register: BX BX BP BP SI DI BP BX */
        const unsigned mod = m >> 6, rm = m & 7u;
        EaEnt *e = &g_eat[m];
        const unsigned direct = (rm == 6u && mod == 0);
        const unsigned zero = (unsigned)__builtin_offsetof(Pc98Adapter, mz);
        unsigned bp_based = (0x4cu >> rm) & 1u;
        e->o1 = direct ? (uint16_t)zero : (uint16_t)(__builtin_offsetof(Pc98Adapter, s.low.r) + 2u * ea1[rm]);
        e->o2 = (!direct && rm < 4u) ? (uint16_t)(__builtin_offsetof(Pc98Adapter, s.low.r) + 2u * (6u + (rm & 1u))) : (uint16_t)zero;
        e->dlen = direct ? 2 : mod == 1 ? 1 : mod == 2 ? 2 : 0;
        e->mk8 = (!direct && mod == 1) ? 0xffffffffu : 0u;
        e->mk16 = (direct || mod == 2) ? 0xffffffffu : 0u;
        e->bo = (uint16_t)(__builtin_offsetof(Pc98Adapter, s.low.base) + 4u * ((!direct && bp_based) ? SG_SS : SG_DS));
    }
#if F16_ASM
    f16x_init();
#endif
    mtab_ready = 1;
}
