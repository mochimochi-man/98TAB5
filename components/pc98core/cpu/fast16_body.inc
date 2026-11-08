/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * fast16 run loop + out-of-line handlers, compiled once per CPU model by fast16.c:
 * F16_MODEL 0 = V30 (menu 0), 1 = 286 (menu 1), 2 = 286 + 386 extensions (menu 2),
 * F16_SFX = the function-name suffix. v30 / menu 2 are compile-time constants here. */
/* F16_MODEL 1 also serves the V30 (menu 0): v30 is a run-time value there (it only matters in a few instructions);
 * F16_MODEL 0 is no longer instantiated (IRAM budget, stage 13-3). */
#if F16_MODEL == 1
#define F16_V30(A) ((A)->menu == 0)
#else
#define F16_V30(A) (F16_MODEL == 0)
#endif
#define F16N3(n, s) n##_##s
#define F16N2(n, s) F16N3(n, s)
#define F16N(n) F16N2(n, F16_SFX)
#define h_misc F16N(h_misc)
#define h_ctl F16N(h_ctl)
#define h_grp F16N(h_grp)
#define h_bcd F16N(h_bcd)
#define h_str F16N(h_str)
#define pc98_fast16_run_m F16N(pc98_fast16_run)

/* ---- out-of-line opcode handlers: return 0 = executed (see x->nip, clk, ...), 1 = refused ---- */
#define H_LOAD() \
    Pc98Adapter *const A = x->A; Pc98Cpu16State *const c = &A->s.low; uint16_t *const r = c->r; uint8_t *const ram = A->ram; \
    uint8_t *const hma = x->hma; const unsigned v30 = F16_V30(A), ext = (A)->menu == 2, mi = v30 ? 0u : 1u; \
    const uint8_t *const clkm = clock_main[mi]; const uint8_t *const p = x->p; \
    const uint8_t *const ckn_ = ckn[mi], *const ckm_ = ckm2[mi]; \
    int32_t *const remclk = A->slice_left_p; \
    int32_t rem = x->rem; const int32_t off = x->off; \
    unsigned fl = x->fl, sy = 0, stop = 0, n = x->n, ip0 = x->ip0, ovr = x->ovr, rep = x->rep, op = x->op, \
        mod = x->xmod, reg = x->xreg, rm = x->xrm, ea = x->ea, padd = 0, refresh = 0, clk = 0, nip = 0, completed = 1, iters = 1, sp = r[4]; \
    uint32_t lin = x->lin; const uint32_t cs_lin = x->cs_lin; \
    (void)remclk; (void)sy; (void)ovr; (void)rep; (void)reg; (void)rm; (void)ea; (void)lin; (void)cs_lin; (void)refresh; \
    (void)ext; (void)mi; (void)clkm; (void)ckn_; (void)ckm_; (void)padd; (void)sp; (void)v30; (void)ram; (void)hma; (void)p; (void)mod; (void)iters; (void)completed
#define H_SAVE() do { x->fl = fl; x->stop = stop; x->n = n; x->nip = nip; x->clk = clk; x->completed = completed; \
        x->iters = iters; x->rem = rem + (int32_t)padd; x->refresh = refresh; } while (0)
#define BAIL() goto h_bail
#define NEXT do { nip = (ip0 + n) & 0xffffu; goto h_out; } while (0)
#define JUMP(t) do { nip = (unsigned)(t) & 0xffffu; goto h_out; } while (0)
#define H_END() h_out: H_SAVE(); return 0; h_bail: H_SAVE(); return 1

__attribute__((optimize("jump-tables", "tree-switch-conversion"))) static __attribute__((noinline)) int h_misc(F16Ctx *x)
{
    H_LOAD();
    switch (op) {
    case 0x60: {
        unsigned orig = sp, k;
        if (sp != 0 && sp < 16u) BAIL();
        for (k = 0; k < 8; k++) PUSHW(k == 4 ? orig : r[k]);
        r[4] = (uint16_t)sp;
        clk = CLKN(); NEXT;
    }
    case 0x61: {
        unsigned k;
        if (sp > 0xfff0u) BAIL();
        for (k = 8; k > 0; k--) {
            unsigned v = RDW(SSB + sp);
            sp = (sp + 2u) & 0xffffu;
            if (k - 1 != 4) r[k - 1] = (uint16_t)v;
        }
        r[4] = (uint16_t)sp;
        clk = CLKN(); NEXT;
    }
    case 0x8f: {
        unsigned v;
        if (reg) BAIL();
        WCHK();
        if (sp == 0xffffu) BAIL();
        v = RDW(SSB + sp);
        r[4] = (uint16_t)(sp + 2u);
        PUT_RM16(v);
        clk = CLKM(); NEXT;
    }
    case 0xc8: {
        unsigned alloc = IMM16(), lvl = IMM8() & 31u, end, bp;
        if (lvl) BAIL();
        if (sp == 1u) BAIL();
        end = (sp - 2u) & 0xffffu;
        if (alloc > end) BAIL();
        bp = r[5];
        WRW(SSB + end, bp);
        r[5] = (uint16_t)end;
        r[4] = (uint16_t)(end - alloc);
        clk = CLKN(); NEXT;
    }
    case 0xc9: {
        unsigned bp = r[5], v;
        if (bp == 0xffffu) BAIL();
        v = RDW(SSB + bp);
        r[4] = (uint16_t)(bp + 2u);
        r[5] = (uint16_t)v;
        clk = CLKN(); NEXT;
    }
    case 0xc4: case 0xc5: {
        unsigned v, sel;
        if (mod == 3 || ea > 0xfffcu) BAIL();
        clk = CLKM();
        v = RDW(lin); sel = RDW(lin + 2u);
        LOADSEG(op == 0xc4 ? SG_ES : SG_DS, sel);
        r[reg] = (uint16_t)v;
        NEXT;
    }
    case 0x86: case 0x87: {
        clk = CLKM();
        if (op & 1u) {
            unsigned v, rv = r[reg];
            WCHK();
            v = RM16();
            PUT_RM16(rv);
            r[reg] = (uint16_t)v;
        } else {
            unsigned v, rv = GET8(reg);
            v = RM8();
            PUT_RM8(rv);
            SET8(reg, v);
        }
        NEXT;
    }
    case 0xd7: {
        unsigned off2 = (r[3] + (r[0] & 255u)) & 0xffffu;
        uint32_t a = c->base[ovr != 4 ? ovr : SG_DS] + off2;
        SET8(0, RDB(a));
        clk = CLKN(); NEXT;
    }
    default: { /* 0x69 / 0x6b IMUL r16, r/m16, imm */
        unsigned factor, src;
        int32_t pr;
        WCHK();
        factor = op == 0x69 ? IMM16() : SX8(IMM8());
        clk = CLKM();
        src = RM16();
        pr = (int32_t)(int16_t)src * (int32_t)(int16_t)factor;
        fl = (fl & ~(unsigned)(F_CF | F_OF)) | (pr != (int32_t)(int16_t)pr ? (F_CF | F_OF) : 0u);
        r[reg] = (uint16_t)pr;
        NEXT;
    }
    }
    H_END();
}

/* far transfers, INT, IRET */
__attribute__((optimize("jump-tables", "tree-switch-conversion"))) static __attribute__((noinline)) int h_ctl(F16Ctx *x)
{
    H_LOAD();
    switch (op) {
    case 0xea: {
        unsigned tgt = IMM16(), sel = IMM16();
        LOADSEG(SG_CS, sel);
        clk = CLKN(); JUMP(tgt);
    }
    case 0x9a: {
        unsigned tgt = IMM16(), sel = IMM16();
        if (sp != 0 && sp < 4u) BAIL();
        PUSHW(c->seg[SG_CS]); PUSHW((ip0 + n) & 0xffffu); r[4] = (uint16_t)sp;
        LOADSEG(SG_CS, sel);
        clk = CLKN(); JUMP(tgt);
    }
    case 0xca: case 0xcb: {
        unsigned rel = op == 0xca ? IMM16() : 0, tgt, sel;
        if (sp > 0xfffcu) BAIL();
        tgt = RDW(SSB + sp); sel = RDW(SSB + sp + 2u);
        r[4] = (uint16_t)(sp + 4u + rel);
        LOADSEG(SG_CS, sel);
        clk = CLKN(); JUMP(tgt);
    }
    case 0xcf: {
        unsigned tgt, sel, f;
        if (sp > 0xfffau) BAIL();
        tgt = RDW(SSB + sp); sel = RDW(SSB + sp + 2u); f = RDW(SSB + sp + 4u);
        r[4] = (uint16_t)(sp + 6u);
        LOADSEG(SG_CS, sel);
        fl = (f & (ext ? 0x7fd5u : 0xfd5u)) | 2u;
        if (ext) { c->cpl = 0; c->cpl_valid = 1; }
        c->nmi_active = 0;
        IRQ_END_CHK();
        clk = CLKN(); JUMP(tgt);
    }
    default: { /* cc cd ce */
        unsigned vec = op == 0xcd ? IMM8() : op == 0xcc ? 3u : 4u, tgt, sel, fv;
        clk = CLKN();
        if (op == 0xce && !(fl & F_OF)) NEXT;
        if (sp != 0 && sp < 6u) BAIL();
        fv = v30 ? ((fl & 0xfd5u) | 2u | 0xf000u) : ((fl & 0x7fd5u) | 2u);
        PUSHW(fv); PUSHW(c->seg[SG_CS]); PUSHW((ip0 + n) & 0xffffu);
        r[4] = (uint16_t)sp;
        fl &= ~(unsigned)(F_IF | F_TF);
        tgt = RDW(vec * 4u); sel = RDW(vec * 4u + 2u);
        LOADSEG(SG_CS, sel);
        c->irq_inhibit = c->nmi_inhibit = 0;
        if (!v30) c->debug_inhibit = 0;
        JUMP(tgt);
    }
    }
    H_END();
}

/* F6 F7 reg>=1 (the rest is inline), FE/FF far and odd forms */
__attribute__((optimize("jump-tables", "tree-switch-conversion"))) static __attribute__((noinline)) int h_grp(F16Ctx *x)
{
    H_LOAD();
    if (op >= 0xfe) {
        unsigned v;
        clk = CLKMF();
        switch (reg) {
        case 3: case 5: {
            unsigned sel;
            if (mod == 3 || ea > 0xfffcu) BAIL();
            if (reg == 3 && sp != 0 && sp < 4u) BAIL();
            v = RDW(lin); sel = RDW(lin + 2u);
            if (reg == 3) { PUSHW(c->seg[SG_CS]); PUSHW((ip0 + n) & 0xffffu); r[4] = (uint16_t)sp; }
            LOADSEG(SG_CS, sel);
            JUMP(v);
        }
        default: BAIL();
        }
    } else {
        unsigned bits = (op & 1u) ? 16u : 8u, v;
        if (reg == 1) BAIL();
        clk = CLKMF();
        if (bits == 16) WCHK();
        v = bits == 16 ? RM16() : RM8();
        if (reg >= 4) {
            unsigned fl2 = fl;
            if (!f_muldiv(reg, bits, r, v, &fl2)) BAIL();
            fl = fl2;
            NEXT;
        }
        BAIL();
    }
    H_END();
}

/* 27 2F 37 3F D4 D5 */
__attribute__((optimize("jump-tables", "tree-switch-conversion"))) static __attribute__((noinline)) int h_bcd(F16Ctx *x)
{
    H_LOAD();
    uint16_t f16 = (uint16_t)fl;
    if (op < 0xd4u) {
        r[0] = v30 ? pc98_cpu_adjust_v30(op, r[0], &f16) : pc98_cpu_adjust286(op, r[0], &f16);
    } else {
        unsigned base = IMM8();   /* V30 ignores the immediate (base 10) */
        if (v30) {
            r[0] = pc98_cpu_adjust_v30(op, r[0], &f16);
        } else {
            uint16_t val;
            if (!pc98_cpu_adjust_base286(op, r[0], (uint8_t)base, &f16, &val)) BAIL();
            r[0] = val;
        }
    }
    fl = f16; clk = CLKN(); NEXT;
    H_END();
}

/* string instructions, INS/OUTS */
__attribute__((optimize("jump-tables", "tree-switch-conversion"))) static __attribute__((noinline)) int h_str(F16Ctx *x)
{
    H_LOAD();
    const unsigned w = (op & 1u) ? 2u : 1u;
    const unsigned io = op < 0xa0u;
    const unsigned use_src = io ? (op >= 0x6eu) : (op <= 0xa7u || op == 0xac || op == 0xad);
    const unsigned use_dst = io ? (op < 0x6eu) : (op <= 0xabu || op >= 0xaeu);
    const unsigned stores = io ? (op < 0x6eu) : (op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab);
    const uint32_t sbase = c->base[ovr != 4 ? ovr : SG_DS], ebase = c->base[SG_ES];
    const unsigned df = (fl & F_DF) != 0;
    unsigned cx = r[1], si = r[6], di = r[7], setup = 0, iter_clk = 0, again = 0, v, a, b;
    iters = 0;
    if (rep && cx == 0) { iters = 1; nip = (ip0 + n) & 0xffffu; clk = clock_rep_zero[mi][op]; goto h_out; }
    if (rep) {
        iter_clk = clock_rep_iter[mi][op];
        setup = (A->rep_valid && A->rep_ip == (uint16_t)ip0) ? 0u : (unsigned)clock_rep_zero[mi][op];
    } else iter_clk = CLKN();
    /* REP MOVS / STOS fast loops: even word addresses, no 64K wrap, source in plain RAM, destination
     * plain RAM (any build) or anything when the clock is live (A->slice_left_p), no rewrite of this very
     * instruction. The reference's per-iteration checks are decided up front for the whole run. */
    if (rep && (op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab) &&
        !(w == 2u && ((use_src && (si & 1u)) || (di & 1u)))) {
        unsigned n_it = cx, lim, nb, k = 0;
        int32_t room = rem - (int32_t)setup;
        uint32_t dlo, dhi, slo = 0, shi = 0, a0, a1, cl = cs_lin;
        const unsigned wsh = w >> 1;   /* w is 1 or 2: shifts instead of divisions */
        lim = df ? (di >> wsh) + 1u : (0x10000u - di) >> wsh;
        if (n_it > lim) n_it = lim;
        if (use_src) { lim = df ? (si >> wsh) + 1u : (0x10000u - si) >> wsh; if (n_it > lim) n_it = lim; }
        if ((int32_t)(n_it * iter_clk) > room) {   /* only a budget that runs out inside this run needs the division */
            nb = room > 0 ? (unsigned)(room + (int32_t)iter_clk - 1) / iter_clk : 1u;
            if (n_it > nb) n_it = nb;
        }
        dlo = df ? ebase + di - (n_it - 1u) * w : ebase + di;
        dhi = dlo + n_it * w;
        if (use_src) { slo = df ? sbase + si - (n_it - 1u) * w : sbase + si; shi = slo + n_it * w; }
        a0 = dlo; a1 = dhi;
        if (!hma) { cl &= 0xfffffu; a0 &= 0xfffffu; a1 = a0 + n_it * w; }
        if (!(a1 > cl && a0 < cl + n) && (!use_src || shi <= 0xa4000u) && (dhi <= 0xa0000u || remclk)) {
            const int step = df ? -(int)w : (int)w;
            uint32_t da = ebase + di, sa = sbase + si;
            const unsigned val = r[0];
            if (dhi <= 0xa0000u) {
                for (; k < n_it; k++) {
                    unsigned v0, v1 = 0;
                    if (use_src) { v0 = ram[sa]; if (w == 2u) v1 = ram[sa + 1u]; }
                    else { v0 = val & 255u; v1 = (val >> 8) & 255u; }
                    F16_HOOK(da, v0); ram[da] = (uint8_t)v0;
                    if (w == 2u) { F16_HOOK(da + 1u, v1); ram[da + 1u] = (uint8_t)v1; }
                    sa += (uint32_t)step; da += (uint32_t)step;
                }
                padd = setup + k * iter_clk;
                rem -= (int32_t)padd;
            } else {
                /* device memory: publish the clock once, then each access (which may add its own wait
                 * states to g_cpu.slice_left) is followed by our own debit */
                A->s.low.flags = (uint16_t)fl;
                *remclk = rem + off;
                if (A->vpage && ((dlo >= 0xa8000u && dhi <= 0xc0000u) || (dlo >= 0xe0000u && dhi <= 0xe8000u))) {
                    /* graphics VRAM (plain / GRCG): the whole run through one call (same effects as the per-word device writes) */
                    const int vm = A->vpage(dlo);
                    if (vm >= 0) k = A->vrun(da, step, n_it, w, use_src ? ram : NULL, sa, val, (unsigned)vm, (int32_t)(iter_clk + setup), (int32_t)iter_clk, remclk, off);
                    if (k) { sa += (uint32_t)(step * (int)k); da += (uint32_t)(step * (int)k); }
                }
                while (k < n_it && !(k && (int32_t)(*remclk - off) <= 0)) {
                    unsigned v0, v1 = 0, acc = iter_clk + (k == 0 ? setup : 0u);
                    if (use_src) { v0 = ram[sa]; if (w == 2u) v1 = ram[sa + 1u]; }
                    else { v0 = val & 255u; v1 = (val >> 8) & 255u; }
                    if (w == 2u) {
                        unsigned v = v0 | (v1 << 8);
                        if (da < 0x9ffffu) { F16_HOOK(da, v0); F16_HOOK(da + 1u, v1); ram[da] = (uint8_t)v0; ram[da + 1u] = (uint8_t)v1; }
                        else if (da - 0x100000u < 0xffffu && hma) { uint8_t *hp = hma + (da - 0x100000u); F16_HOOK(da, v0); F16_HOOK(da + 1u, v1); hp[0] = (uint8_t)v0; hp[1] = (uint8_t)v1; }
                        else if (A->devww) A->devww(da, v);
                        else A->bus.write16(A->bus.context, da, (uint16_t)v);
                    } else {
                        if (da < 0xa0000u) { F16_HOOK(da, v0); ram[da] = (uint8_t)v0; }
                        else if (da - 0x100000u < 0x10000u && hma) { F16_HOOK(da, v0); hma[da - 0x100000u] = (uint8_t)v0; }
                        else if (A->devwb) A->devwb(da, v0);
                        else A->bus.write8(A->bus.context, da, (uint8_t)v0);
                    }
                    sa += (uint32_t)step; da += (uint32_t)step; k++;
                    *remclk -= (int32_t)acc;
                    if ((int32_t)(*remclk - off) <= 0) break;
                }
                padd = setup + k * iter_clk;
                rem = (int32_t)*remclk - off;
                refresh = 1;
            }
            if (use_src) si = (si + (unsigned)(step * (int)k)) & 0xffffu;
            di = (di + (unsigned)(step * (int)k)) & 0xffffu;
            cx -= k; iters = k; again = cx != 0;
            goto str_done;
        }
    }
    /* REPE/REPNE CMPS/SCAS over plain RAM: compare loop without the per-iteration bookkeeping. Only the last
     * compare's flags survive, so the flags are computed once from the final pair. */
    if (rep && (op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) &&
        !(w == 2u && ((use_src && (si & 1u)) || (di & 1u)))) {
        unsigned n_it = cx, lim, nb;
        int32_t room = rem - (int32_t)setup;
        uint32_t dlo, dhi, slo = 0, shi = 0;
        const unsigned wsh = w >> 1;
        lim = df ? (di >> wsh) + 1u : (0x10000u - di) >> wsh;
        if (n_it > lim) n_it = lim;
        if (use_src) { lim = df ? (si >> wsh) + 1u : (0x10000u - si) >> wsh; if (n_it > lim) n_it = lim; }
        if ((int32_t)(n_it * iter_clk) > room) {
            nb = room > 0 ? (unsigned)(room + (int32_t)iter_clk - 1) / iter_clk : 1u;
            if (n_it > nb) n_it = nb;
        }
        dlo = df ? ebase + di - (n_it - 1u) * w : ebase + di;
        dhi = dlo + n_it * w;
        if (use_src) { slo = df ? sbase + si - (n_it - 1u) * w : sbase + si; shi = slo + n_it * w; }
        if (dhi <= 0xa0000u && (!use_src || shi <= 0xa0000u)) {
            const int step = df ? -(int)w : (int)w;
            const unsigned want = (rep == 0xf3u);
            const uint8_t *dp = ram + ebase + di, *sp = ram + sbase + si;
            unsigned k = 0, eq;
            if (w == 1u) {
                const unsigned acc8 = r[0] & 255u;
                do {
                    a = use_src ? sp[0] : acc8; b = dp[0];
                    sp += step; dp += step; k++;
                    eq = (a == b);
                } while (k < n_it && eq == want);
                (void)f_sub8(a, b, 0, &fl);
            } else {
                const unsigned acc16 = r[0];
                do {
                    a = use_src ? (unsigned)(sp[0] | (sp[1] << 8)) : acc16; b = (unsigned)(dp[0] | (dp[1] << 8));
                    sp += step; dp += step; k++;
                    eq = (a == b);
                } while (k < n_it && eq == want);
                (void)f_sub16(a, b, 0, &fl);
            }
            if (use_src) si = (si + (unsigned)(step * (int)k)) & 0xffffu;
            di = (di + (unsigned)(step * (int)k)) & 0xffffu;
            cx -= k; iters = k;
            again = (cx != 0) && (eq == want);
            padd = setup + k * iter_clk;
            rem -= (int32_t)padd;
            goto str_done;
        }
    }
    for (;;) {
        unsigned acc;
        if (w == 2 && ((use_src && si == 0xffffu) || (use_dst && di == 0xffffu))) {
            if (!iters) BAIL();
            again = 1; break;
        }
        switch (op) {
        case 0x6c: case 0x6d:
            sy = ~stop & EXT; stop |= EXT | IOX;
            v = slow_in(A, r[2], w == 2, SLOWARGS);
            if (w == 2) WRW(ebase + di, v); else WRB(ebase + di, v);
            break;
        case 0x6e: v = RDB(sbase + si); sy = ~stop & EXT; stop |= EXT | IOX; slow_out(A, r[2], 0, v, SLOWARGS); break;
        case 0x6f: v = RDW(sbase + si); sy = ~stop & EXT; stop |= EXT | IOX; slow_out(A, r[2], 1, v, SLOWARGS); break;
        case 0xa4: v = RDB(sbase + si); WRB(ebase + di, v); break;
        case 0xa5: v = RDW(sbase + si); WRW(ebase + di, v); break;
        case 0xa6: a = RDB(sbase + si); b = RDB(ebase + di); (void)f_sub8(a, b, 0, &fl); break;
        case 0xa7: a = RDW(sbase + si); b = RDW(ebase + di); (void)f_sub16(a, b, 0, &fl); break;
        case 0xaa: WRB(ebase + di, r[0]); break;
        case 0xab: WRW(ebase + di, r[0]); break;
        case 0xac: SET8(0, RDB(sbase + si)); break;
        case 0xad: r[0] = (uint16_t)RDW(sbase + si); break;
        case 0xae: b = RDB(ebase + di); (void)f_sub8(r[0] & 255u, b, 0, &fl); break;
        default:   b = RDW(ebase + di); (void)f_sub16(r[0], b, 0, &fl); break;
        }
        if (use_src) si = (si + (df ? 0x10000u - w : w)) & 0xffffu;
        if (use_dst) di = (di + (df ? 0x10000u - w : w)) & 0xffffu;
        iters++;
        acc = iter_clk + (iters == 1 ? setup : 0u);
        if (remclk && (stop & EXT) && !io) {
            /* a device may have retargeted g_cpu.slice_left during this iteration: continue from its live value */
            rem = *remclk - off; stop &= ~EXT; refresh = 1;
        }
        rem -= (int32_t)acc; padd += acc;
        if (!rep) break;
        cx = (cx - 1u) & 0xffffu;
        again = cx != 0;
        if (again && (op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf)) again = (rep == 0xf3) == ((fl & F_ZF) != 0);
        if (!again) break;
        /* continue only inside the budget, never across port I/O, and never after
         * the iteration rewrote its own instruction bytes (REP re-fetches) */
        if (io || (stop & EXT) || rem <= 0) break;
        if (stores) {
            uint32_t wa = ebase + ((di - (df ? 0x10000u - w : w)) & 0xffffu);
            uint32_t cl = cs_lin;
            if (!hma) { wa &= 0xfffffu; cl &= 0xfffffu; }   /* A20 closed (or unknown): the 1MB alias is the same memory */
            if (wa + w > cl && wa < cl + n) break;
        }
    }
str_done:
    if (rep) r[1] = (uint16_t)cx;
    if (use_src) r[6] = (uint16_t)si;
    if (use_dst) r[7] = (uint16_t)di;
    clk = padd;
    if (rep && again) {
        nip = ip0; completed = 0;
        A->rep_valid = 1; A->rep_ip = (uint16_t)ip0;
        goto h_out;
    }
    if (rep) A->rep_valid = 0;
    NEXT;
    H_END();
}

/* ---- the dispatcher ---- */
#undef BAIL
#undef NEXT
#undef JUMP
#define BAIL() goto bail
#define NEXT do { nip = ip0 + n; goto commit; } while (0)   /* ip0 < 0xfff0 and n <= 10 here: no wrap */
#define JUMP(t) do { nip = (unsigned)(t) & 0xffffu; goto commit; } while (0)
#define ALU16K(K, a, b) ((K) == 0 ? f_add16(a, b, 0, &fl) : (K) == 4 ? f_log16((a) & (b), &fl) : (K) == 5 ? f_sub16(a, b, 0, &fl) : f_sub16(a, b, 0, &fl))
#define ALU8K(K, a, b) ((K) == 0 ? f_add8(a, b, 0, &fl) : (K) == 4 ? f_log8((a) & (b), &fl) : (K) == 5 ? f_sub8(a, b, 0, &fl) : f_sub8(a, b, 0, &fl))
/* hot ALU ops inline (ADD AND SUB CMP), the rest through the shared function */
#define ALU16X(aop, a, b, res) switch (aop) { \
        case 0: res = ALU16K(0, a, b); break; case 4: res = ALU16K(4, a, b); break; \
        case 5: res = ALU16K(5, a, b); break; case 7: res = ALU16K(7, a, b); break; \
        case 1: res = f_log16((a) | (b), &fl); break; case 6: res = f_log16((a) ^ (b), &fl); break; \
        case 2: res = f_add16(a, b, fl & F_CF, &fl); break; case 3: res = f_sub16(a, b, fl & F_CF, &fl); break; \
        default: res = alu16(aop, a, b, &fl); break; }
#define ALU8X(aop, a, b, res) switch (aop) { \
        case 0: res = ALU8K(0, a, b); break; case 4: res = ALU8K(4, a, b); break; \
        case 5: res = ALU8K(5, a, b); break; case 7: res = ALU8K(7, a, b); break; \
        case 1: res = f_log8((a) | (b), &fl); break; case 6: res = f_log8((a) ^ (b), &fl); break; \
        case 2: res = f_add8(a, b, fl & F_CF, &fl); break; case 3: res = f_sub8(a, b, fl & F_CF, &fl); break; \
        default: res = alu8(aop, a, b, &fl); break; }

/* shift/rotate group: one arm per operation so the operation and the width are compile-time constants */
#define SHIFT1_ARMS(B, v, res) switch (reg) {         case 0: res = f_shift1(0, B, v, &fl); break; case 1: res = f_shift1(1, B, v, &fl); break;         case 2: res = f_shift1(2, B, v, &fl); break; case 3: res = f_shift1(3, B, v, &fl); break;         case 4: res = f_shift1(4, B, v, &fl); break; case 5: res = f_shift1(5, B, v, &fl); break;         default: res = f_shift1(7, B, v, &fl); break; }
#define SHIFTN_ARMS(B, v, res) switch (reg) {         case 0: res = cnt == 1u ? f_shift1(0, B, v, &fl) : f_shiftN(0, B, v, cnt, &fl); break;         case 1: res = cnt == 1u ? f_shift1(1, B, v, &fl) : f_shiftN(1, B, v, cnt, &fl); break;         case 2: res = cnt == 1u ? f_shift1(2, B, v, &fl) : f_shiftN(2, B, v, cnt, &fl); break;         case 3: res = cnt == 1u ? f_shift1(3, B, v, &fl) : f_shiftN(3, B, v, cnt, &fl); break;         case 4: res = cnt == 1u ? f_shift1(4, B, v, &fl) : f_shiftN(4, B, v, cnt, &fl); break;         case 5: res = cnt == 1u ? f_shift1(5, B, v, &fl) : f_shiftN(5, B, v, cnt, &fl); break;         default: res = cnt == 1u ? f_shift1(7, B, v, &fl) : f_shiftN(7, B, v, cnt, &fl); break; }

/* the ModR/M fields are derived on use (nothing is computed for opcodes that do not need them) */
#define mod (modrm >> 6)
#define reg ((modrm >> 3) & 7u)
#define rm (modrm & 7u)
/* ModR/M + effective address, expanded at the start of each self-decoding case (see F16_SELFDEC) */
#define DEC_MODRM() ((void)0)
__attribute__((optimize("jump-tables", "tree-switch-conversion")))
int pc98_fast16_run_m(Pc98Adapter *A, int32_t budget, unsigned max_insn, unsigned *clocks)
{
    Pc98Cpu16State *const c = &A->s.low;
    uint16_t *const r = c->r;
    uint8_t *const ram = A->ram;
    uint8_t *hma;
    int32_t *const remclk = A->slice_left_p;
    const unsigned v30 = F16_V30(A), ext = A->menu == 2;
    const unsigned mi = v30 ? 0u : 1u;
    const uint8_t *const clkm = clock_main[mi];
    const uint8_t *const opt = ext ? optab2 : optab;
    const uint8_t *const ckn_ = ckn[mi], *const ckm_ = ckm2[mi];
    int32_t rem = budget, off = 0;
    unsigned fl = c->flags, stop, inh, nret = 0, sy = 0, anyc = 0;
    unsigned ip0 = c->ip, n, ovr, rep, op, cls, clk = 0, nip = 0, completed = 1, iters;
    unsigned modrm = 0xc0u, ea = 0, dseg = SG_DS, sp;
    uint32_t lin = 0, cs_lin;
    const uint8_t *p;
    uint8_t win[10];
    F16Ctx x;

    if (!optab_ready) optab_init();
    if (!mtab_ready) mini_init();
    if (A->mck_mi != mi + 1u) mini_models(A, mi);
    if (__builtin_expect(A->r8o[1] == 0, 0)) {
        unsigned k;
        for (k = 0; k < 8; k++) A->r8o[k] = (uint8_t)(((uintptr_t)A->s.low.r - (uintptr_t)A) + (((k & 3u) << 1) | (k >> 2)));
    }
    HMA_REFRESH();
    if (remclk) off = *remclk - budget;
    stop = (max_insn == 1u || budget <= 1 || A->pending) ? HARD : 0u;
    inh = c->irq_inhibit | c->nmi_inhibit | c->debug_inhibit;
    (void)sy;
    if (!stop && !inh) goto mini_enter;
next_insn:
    ovr = 4; rep = 0;
    if (ip0 >= 0xfff0u) goto bail;
    cs_lin = c->base[SG_CS] + ip0;
    if (__builtin_expect(cs_lin <= 0xa3ff6u, 1)) p = ram + cs_lin;
    else if (cs_lin - 0x100000u <= 0xfff6u && hma) p = hma + (cs_lin - 0x100000u);
    else if (A->rom && cs_lin >= 0xe8000u && cs_lin <= 0xffff6u && !*A->itf) p = A->rom + cs_lin;   /* BIOS ROM area: plain mem[] reads */
    else if (A->rom && cs_lin >= 0xf8000u && cs_lin <= 0xffff6u) p = A->rom + cs_lin + F16_ITF_STEP;   /* ITF bank: mem[addr + GVRAM_PAGE1] */
    else {
        slow_fetch(A, cs_lin, win, fl, (int32_t)(rem + off));
        stop |= EXT;
        p = win;
    }

    op = p[0]; n = 1; cls = opt[op];
    if (cls) {
      if (__builtin_expect(cls != 1u, 0)) {   /* cls 1 (ModR/M only, the common case) skips the refuse / prefix checks */
        if (__builtin_expect(cls & 4u, 0)) goto bail;
        /* prefixes (at most 4; segment overrides and REP) */
        if (__builtin_expect(cls & 2u, 0)) {
            do {
                if ((op & 0xfeu) == 0xf2u) rep = op;
                else ovr = (op >> 3) & 3u;   /* 26 ES(0), 2E CS(1), 36 SS(2), 3E DS(3) */
                if (n >= 5u) goto bail;
                op = p[n++];
            } while (opt[op] & 2u);
            cls = opt[op];
            if (__builtin_expect(cls & 4u, 0)) goto bail;
            if (rep && !((op >= 0x6cu && op <= 0x6fu) || (op >= 0xa4u && op <= 0xa7u) || (op >= 0xaau && op <= 0xafu))) goto bail;
        }
      }
        if (cls & 1u) {
            modrm = IMM8();
            if (modrm < 0xc0u) {
                if ((modrm & 0xc7u) == 0x46u) { ea = (r[5] + (unsigned)(int)(int8_t)p[n++]) & 0xffffu; dseg = SG_SS; }          /* [BP+disp8] */
                else if ((modrm & 0xc7u) == 0x06u) { ea = (unsigned)p[n] | ((unsigned)p[n + 1u] << 8); n += 2u; dseg = SG_DS; }  /* [disp16] */
                else ea = ea_calc(r, mod, rm, p, &n, &dseg);
                lin = c->base[ovr != 4 ? ovr : dseg] + ea;
            }
        }
    }

    switch ((uint8_t)op) {
    /* ---- ALU r/m,r  r,r/m  acc,imm ---- */
    /* one case per opcode: the operation and the form are compile-time constants inside each */
#define ALU_BODY(AOP, FORM) {         const unsigned aop = (AOP), form = (FORM);         unsigned res, a, b;         if (form >= 4) {             if (form == 4) { b = IMM8(); a = r[0] & 255u; ALU8X(aop, a, b, res); if (aop != 7) SET8(0, res); }             else { b = IMM16(); a = r[0]; ALU16X(aop, a, b, res); if (aop != 7) r[0] = (uint16_t)res; }             clk = CLKN(); NEXT;         }         clk = CLKM();         if (form & 1u) {             unsigned rv, mv;             WCHK();             rv = r[reg]; mv = RM16();             if (form & 2u) { ALU16X(aop, rv, mv, res); if (aop != 7) r[reg] = (uint16_t)res; }             else { ALU16X(aop, mv, rv, res); if (aop != 7) PUT_RM16(res); }         } else {             unsigned rv = GET8(reg), mv = RM8();             if (form & 2u) { ALU8X(aop, rv, mv, res); if (aop != 7) SET8(reg, res); }             else { ALU8X(aop, mv, rv, res); if (aop != 7) PUT_RM8(res); }         }         NEXT; }
#define ALU_ROW(b, AOP) case (b): ALU_BODY(AOP, 0) case (b) + 1: ALU_BODY(AOP, 1) case (b) + 2: ALU_BODY(AOP, 2)         case (b) + 3: ALU_BODY(AOP, 3) case (b) + 4: ALU_BODY(AOP, 4) case (b) + 5: ALU_BODY(AOP, 5)
    ALU_ROW(0x00, 0) ALU_ROW(0x08, 1) ALU_ROW(0x10, 2) ALU_ROW(0x18, 3) ALU_ROW(0x20, 4) ALU_ROW(0x28, 5) ALU_ROW(0x30, 6) ALU_ROW(0x38, 7)
    case 0x80: case 0x81: case 0x82: case 0x83: {
        unsigned res, imm, mv;
        clk = CLKMF();          /* by operation: CMP (/7) does not write back and takes fewer clocks */
        if (op & 1u) {
            WCHK();
            imm = op == 0x83 ? SX8(IMM8()) : IMM16();
            mv = RM16();
            ALU16X(reg, mv, imm, res);
            if (reg != 7) PUT_RM16(res);
        } else {
            imm = IMM8();
            mv = RM8();
            ALU8X(reg, mv, imm, res);
            if (reg != 7) PUT_RM8(res);
        }
        NEXT;
    }
    case 0x84: case 0x85:
        clk = CLKM();
        if (op & 1u) { WCHK(); (void)f_log16(RM16() & r[reg], &fl); }
        else (void)f_log8(RM8() & GET8(reg), &fl);
        NEXT;
    case 0xa8: { unsigned b = IMM8(); (void)f_log8((r[0] & 255u) & b, &fl); clk = CLKN(); NEXT; }
    case 0xa9: { unsigned b = IMM16(); (void)f_log16(r[0] & b, &fl); clk = CLKN(); NEXT; }

    /* ---- INC/DEC reg16 ---- */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        r[op & 7u] = (uint16_t)f_inc16(r[op & 7u], &fl); clk = CLKN(); NEXT;
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        r[op & 7u] = (uint16_t)f_dec16(r[op & 7u], &fl); clk = CLKN(); NEXT;

    /* ---- PUSH / POP ---- */
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        unsigned v = r[op & 7u];
        sp = r[4];
        if (sp == 1u) BAIL();
        if (v30 && (op & 7u) == 4u) v = (v - 2u) & 0xffffu;
        PUSHW(v); r[4] = (uint16_t)sp;
        clk = CLKN(); NEXT;
    }
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        unsigned v;
        sp = r[4];
        if (sp == 0xffffu) BAIL();
        v = RDW(SSB + sp);
        r[4] = (uint16_t)(sp + 2u);
        if (v30 && (op & 7u) == 4u) v = (v + 2u) & 0xffffu;
        r[op & 7u] = (uint16_t)v;
        clk = CLKN(); NEXT;
    }
    case 0x68: case 0x6a: {
        unsigned v = op == 0x68 ? IMM16() : SX8(IMM8());
        sp = r[4];
        if (sp == 1u) BAIL();
        PUSHW(v); r[4] = (uint16_t)sp;
        clk = CLKN(); NEXT;
    }
    case 0x06: case 0x0e: case 0x16: case 0x1e:
        sp = r[4];
        if (sp == 1u) BAIL();
        PUSHW(c->seg[op >> 3]); r[4] = (uint16_t)sp;
        clk = CLKN(); NEXT;
    case 0x07: case 0x17: case 0x1f: {
        unsigned v;
        sp = r[4];
        if (sp == 0xffffu) BAIL();
        v = RDW(SSB + sp);
        r[4] = (uint16_t)(sp + 2u);
        LOADSEG_I(op >> 3, v);
        if (op == 0x17) { c->irq_inhibit = c->nmi_inhibit = 2; c->debug_inhibit = 2; inh = 1; }
        clk = CLKN(); NEXT;
    }
    case 0x9c: {
        unsigned v = v30 ? ((fl & 0xfd5u) | 2u | 0xf000u) : ((fl & 0x7fd5u) | 2u);
        sp = r[4];
        if (sp == 1u) BAIL();
        PUSHW(v); r[4] = (uint16_t)sp;
        clk = CLKN(); NEXT;
    }
    case 0x9d: {
        unsigned v;
        sp = r[4];
        if (sp == 0xffffu) BAIL();
        v = RDW(SSB + sp);
        r[4] = (uint16_t)(sp + 2u);
        fl = v30 ? ((v & 0xfd5u) | 2u) : ((v & 0xfd5u) | 2u | (fl & 0x7000u));
        if (fl & F_TF) stop |= HARD;
        IRQ_END_CHK();
        clk = CLKN(); NEXT;
    }

    /* ---- MOV ---- */
    case 0x88: DEC_MODRM(); clk = CLKM(); PUT_RM8(GET8(reg)); NEXT;
    case 0x89: DEC_MODRM(); clk = CLKM(); WCHK(); PUT_RM16(r[reg]); NEXT;
    case 0x8a: DEC_MODRM(); clk = CLKM(); { unsigned v = RM8(); SET8(reg, v); } NEXT;
    case 0x8b: DEC_MODRM(); clk = CLKM(); WCHK(); { unsigned v = RM16(); r[reg] = (uint16_t)v; } NEXT;
    case 0x8c:
        if (reg > 3u) BAIL();
        clk = CLKM(); WCHK();
        PUT_RM16(c->seg[reg]);
        NEXT;
    case 0x8e: {
        unsigned v;
        if (reg > 3u || reg == SG_CS) BAIL();
        clk = CLKMF(); WCHK();
        v = RM16();
        LOADSEG_I(reg, v);
        if (reg == SG_SS) { c->irq_inhibit = c->nmi_inhibit = 2; c->debug_inhibit = 2; inh = 1; }
        NEXT;
    }
    case 0x8d:
        DEC_MODRM();
        if (mod == 3) BAIL();
        clk = CLKM();
        r[reg] = (uint16_t)ea;
        NEXT;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        unsigned off2 = IMM16();
        uint32_t a = c->base[ovr != 4 ? ovr : SG_DS] + off2;
        if ((op & 1u) && off2 == 0xffffu) BAIL();
        clk = CLKN();
        if (op == 0xa0) SET8(0, RDB(a));
        else if (op == 0xa1) r[0] = (uint16_t)RDW(a);
        else if (op == 0xa2) WRB(a, r[0]);
        else WRW(a, r[0]);
        NEXT;
    }
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        SET8(op & 7u, IMM8()); clk = CLKN(); NEXT;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        r[op & 7u] = (uint16_t)IMM16(); clk = CLKN(); NEXT;
    case 0xc6: case 0xc7: {
        unsigned v;
        if (reg) BAIL();
        if (op == 0xc7) { WCHK(); v = IMM16(); clk = CLKM(); PUT_RM16(v); }
        else { v = IMM8(); clk = CLKM(); PUT_RM8(v); }
        NEXT;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = r[op & 7u];
        r[op & 7u] = r[0]; r[0] = t;
        clk = CLKN(); NEXT;
    }
    case 0x98: r[0] = (uint16_t)((r[0] & 255u) | ((r[0] & 128u) ? 0xff00u : 0u)); clk = CLKN(); NEXT;
    case 0x99: r[2] = (r[0] & 0x8000u) ? 0xffffu : 0u; clk = CLKN(); NEXT;
    case 0x9e: fl = (fl & ~0xd5u) | ((r[0] >> 8) & 0xd5u); clk = CLKN(); NEXT;
    case 0x9f: SET8(4, (fl & 0xd5u) | 2u); clk = CLKN(); NEXT;

    /* ---- flag ops ---- */
    case 0xf5: fl ^= F_CF; clk = CLKN(); NEXT;
    case 0xf8: fl &= ~(unsigned)F_CF; clk = CLKN(); NEXT;
    case 0xf9: fl |= F_CF; clk = CLKN(); NEXT;
    case 0xfa: fl &= ~(unsigned)F_IF; clk = CLKN(); NEXT;
    case 0xfb: { unsigned wasdi = !(fl & F_IF); fl |= F_IF; c->irq_inhibit = 2; inh = 1; if (wasdi) IRQ_END_CHK(); clk = CLKN(); NEXT; }
    case 0xfc: fl &= ~(unsigned)F_DF; clk = CLKN(); NEXT;
    case 0xfd: fl |= F_DF; clk = CLKN(); NEXT;

    /* ---- shifts / rotates ---- */
    case 0xd0: case 0xd1: {   /* shift / rotate by 1: the common form, no count handling */
        unsigned v, res;
        if (reg == 6) BAIL();
        if (mod == 3 && !(op & 1u)) {   /* register byte by 1 (rotates in sprite loops) */
            v = GET8(rm);
            if (reg == 0) res = f_shift1(0, 8, v, &fl);
            else if (reg == 1) res = f_shift1(1, 8, v, &fl);
            else if (reg == 2) res = f_shift1(2, 8, v, &fl);
            else SHIFT1_ARMS(8, v, res);
            SET8(rm, res);
            clk = (unsigned)ckm_[0xd0u << 1];
            NEXT;
        }
        clk = CLKM();
        if (op & 1u) {
            WCHK();
            v = RM16();
            SHIFT1_ARMS(16, v, res);
            PUT_RM16(res);
        } else {
            v = RM8();
            SHIFT1_ARMS(8, v, res);
            PUT_RM8(res);
        }
        NEXT;
    }
    case 0xc0: case 0xc1: case 0xd2: case 0xd3: {
        unsigned cnt, extra = 0, v, res;
        if (reg == 6) BAIL();
        if (op == 0xc0u && mod == 3) {   /* register byte by an immediate count (rotates in sprite loops) */
            cnt = IMM8();
            clk = (unsigned)ckm_[0xc0u << 1] + (unsigned)clock_shift[mi][0] * (cnt & 31u);
            if (!v30) cnt &= 31u;
            if (!cnt) NEXT;
            v = GET8(rm);
            if (reg == 0) res = cnt == 1u ? f_shift1(0, 8, v, &fl) : f_shiftN(0, 8, v, cnt, &fl);
            else if (reg == 1) res = cnt == 1u ? f_shift1(1, 8, v, &fl) : f_shiftN(1, 8, v, cnt, &fl);
            else if (reg == 2) res = cnt == 1u ? f_shift1(2, 8, v, &fl) : f_shiftN(2, 8, v, cnt, &fl);
            else SHIFTN_ARMS(8, v, res);
            SET8(rm, res);
            NEXT;
        }
        if (op < 0xd0u) { cnt = IMM8(); extra = (unsigned)clock_shift[mi][op & 1u] * ((unsigned)p[(n > 8u ? 8u : n) - 1u] & 31u); }
        else if (op < 0xd2u) cnt = 1;
        else { cnt = r[1] & 255u; extra = (unsigned)clock_shift[mi][2u + (op & 1u)] * (cnt & 31u); }
        clk = CLKM() + extra;
        if (!v30) cnt &= 31u;
        if (!cnt) { if (!v30 && mod != 3) BAIL(); NEXT; }
        if (op & 1u) {
            WCHK();
            v = RM16();
            SHIFTN_ARMS(16, v, res);
            PUT_RM16(res);
        } else {
            v = RM8();
            SHIFTN_ARMS(8, v, res);
            PUT_RM8(res);
        }
        NEXT;
    }

    /* ---- F6 / F7 TEST NOT NEG, FE, FF ---- */
    case 0xf6: case 0xf7: {
        unsigned imm = 0, v, res;
        if (reg == 1 || reg >= 4) goto handler;
        clk = CLKMF();
        if (op & 1u) {
            WCHK();
            if (reg == 0) imm = IMM16();
            v = RM16();
            if (reg == 0) { (void)f_log16(v & imm, &fl); NEXT; }
            res = reg == 2 ? ~v : f_sub16(0, v, 0, &fl);
            PUT_RM16(res);
        } else {
            if (reg == 0) imm = IMM8();
            v = RM8();
            if (reg == 0) { (void)f_log8(v & imm, &fl); NEXT; }
            res = reg == 2 ? ~v : f_sub8(0, v, 0, &fl);
            PUT_RM8(res);
        }
        NEXT;
    }
    case 0xfe: {
        unsigned v, res;
        if (reg > 1) BAIL();
        clk = CLKM();
        v = RM8();
        res = reg ? f_dec8(v, &fl) : f_inc8(v, &fl);
        PUT_RM8(res);
        NEXT;
    }
    case 0xff: {
        unsigned v;
        if (reg == 3 || reg == 5 || reg == 7) goto handler;
        clk = CLKMF();
        WCHK();
        v = RM16();
        switch (reg) {
        case 0: v = f_inc16(v, &fl); PUT_RM16(v); NEXT;
        case 1: v = f_dec16(v, &fl); PUT_RM16(v); NEXT;
        case 2:
            sp = r[4];
            if (sp == 1u) BAIL();
            PUSHW((ip0 + n) & 0xffffu); r[4] = (uint16_t)sp;
            JUMP(v);
        case 4: JUMP(v);
        default: /* 6 */
            sp = r[4];
            if (sp == 1u) BAIL();
            PUSHW(v); r[4] = (uint16_t)sp;
            NEXT;
        }
    }

    /* ---- branches ---- */
#define JCC(opc, cnd) case opc: { unsigned d = SX8(IMM8()); clk = CLKN(); nip = (ip0 + n) & 0xffffu; \
        if (cnd) { if (d) clk += clock_taken[mi][opc]; nip = (nip + d) & 0xffffu; } goto commit; }
#define SLT() ((((fl) >> 7) ^ ((fl) >> 11)) & 1u)
    JCC(0x70, fl & F_OF)                  JCC(0x71, !(fl & F_OF))
    JCC(0x72, fl & F_CF)                  JCC(0x73, !(fl & F_CF))
    JCC(0x74, fl & F_ZF)                  JCC(0x75, !(fl & F_ZF))
    JCC(0x76, fl & (F_CF | F_ZF))         JCC(0x77, !(fl & (F_CF | F_ZF)))
    JCC(0x78, fl & F_SF)                  JCC(0x79, !(fl & F_SF))
    JCC(0x7a, fl & F_PF)                  JCC(0x7b, !(fl & F_PF))
    JCC(0x7c, SLT())                      JCC(0x7d, !SLT())
    JCC(0x7e, SLT() || (fl & F_ZF))       JCC(0x7f, !(SLT() || (fl & F_ZF)))
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: {
        unsigned d = SX8(IMM8()), fall = (ip0 + n) & 0xffffu, take;
        if (op == 0xe3) take = r[1] == 0;
        else {
            r[1] = (uint16_t)(r[1] - 1u);
            take = r[1] != 0;
            if (op == 0xe0) take = take && !(fl & F_ZF);
            if (op == 0xe1) take = take && (fl & F_ZF);
        }
        clk = CLKN();
        nip = take ? (fall + d) & 0xffffu : fall;
        if (nip != ((ip0 + (n > 8u ? 8u : n)) & 0xffffu)) clk += clock_taken[mi][op];
        goto commit;
    }
    case 0xeb: { unsigned d = SX8(IMM8()); clk = CLKN(); nip = (ip0 + n + d) & 0xffffu; goto commit; }
    case 0xe9: { unsigned d = IMM16(); clk = CLKN(); nip = (ip0 + n + d) & 0xffffu; goto commit; }
    case 0xe8: {
        unsigned d = IMM16(), ret = (ip0 + n) & 0xffffu;
        sp = r[4];
        if (sp == 1u) BAIL();
        PUSHW(ret); r[4] = (uint16_t)sp;
        clk = CLKN(); nip = (ret + d) & 0xffffu; goto commit;
    }
    case 0xc2: case 0xc3: {
        unsigned rel = op == 0xc2 ? IMM16() : 0, v;
        sp = r[4];
        if (sp == 0xffffu) BAIL();
        v = RDW(SSB + sp);
        r[4] = (uint16_t)(sp + 2u + rel);
        clk = CLKN(); JUMP(v);
    }

    /* ---- everything else lives out of line ---- */
    default:
    handler: {
        int (*h)(F16Ctx *);
        int refused;
        switch (op) {
        case 0xc4: case 0xc5:
            if (mod != 3 && ea <= 0xfffcu && lin + 4u <= 0xa4000u) {
                unsigned lv = ram[lin] | ((unsigned)ram[lin + 1u] << 8), ls = ram[lin + 2u] | ((unsigned)ram[lin + 3u] << 8);
                clk = CLKM();
                load_seg(A, op == 0xc4 ? SG_ES : SG_DS, ls);
                r[reg] = (uint16_t)lv;
                NEXT;
            }
            h = h_misc; break;
        case 0x8f:
            sp = r[4];
            if (!reg && sp < 0xfffeu && SSB + sp + 2u <= 0xa4000u && (mod == 3 || (ea != 0xffffu && lin + 2u <= 0xa0000u))) {
                unsigned pv = ram[SSB + sp] | ((unsigned)ram[SSB + sp + 1u] << 8);
                r[4] = (uint16_t)(sp + 2u);
                if (mod == 3) r[rm] = (uint16_t)pv;
                else { F16_HOOK(lin, pv & 255u); F16_HOOK(lin + 1u, pv >> 8); ram[lin] = (uint8_t)pv; ram[lin + 1u] = (uint8_t)(pv >> 8); }
                clk = CLKM();
                NEXT;
            }
            h = h_misc; break;
        case 0x60: case 0x61: case 0x86: case 0x87: case 0xc8: case 0xc9:
        case 0x69: case 0x6b: case 0xd7:
            h = h_misc; break;

        case 0x9a: case 0xea: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
            h = h_ctl; break;
        case 0xf6: case 0xf7: case 0xfe: case 0xff:
            h = h_grp; break;
        case 0x27: case 0x2f: case 0x37: case 0x3f: case 0xd4: case 0xd5:
            h = h_bcd; break;
        case 0xe4: case 0xe5: case 0xe6: case 0xe7: case 0xec: case 0xed: case 0xee: case 0xef:
        {
            unsigned port = op < 0xecu ? IMM8() : r[2], wide = op & 1u;
            clk = CLKN();
            sy = ~stop & EXT; stop |= EXT | (((port - 0xf0u) < 8u) ? IOX : 0u);   /* F0..F7: CPU reset / A20 */
            if (op & 2u) slow_out(A, port, wide, r[0], SLOWARGS);
            else { unsigned v = slow_in(A, port, wide, SLOWARGS); if (wide) r[0] = (uint16_t)v; else SET8(0, v); }
            NEXT;
        }
        case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
        case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
            if (!rep && (op == 0xa4 || op == 0xa5 || (op >= 0xaa && op <= 0xad))) { int rv = str1(A, op, ovr, fl, (int32_t)(rem + off)); if (rv) { if (rv == 2) { if (remclk) rem = (int32_t)*remclk - off; else stop |= HARD; } clk = CLKN(); NEXT; } }
            h = h_str; break;
        default: goto bail;
        }
        x.A = A; x.p = p; x.hma = hma; x.lin = lin; x.cs_lin = cs_lin; x.rem = rem; x.off = off;
        x.fl = fl; x.ip0 = ip0; x.n = n; x.ovr = ovr; x.rep = rep; x.op = op;
        x.xmod = mod; x.xreg = reg; x.xrm = rm; x.ea = ea;
        refused = h(&x);
        fl = x.fl; stop |= x.stop;
        if (refused) goto bail;
        anyc |= x.completed;
        n = x.n; nip = x.nip; clk = x.clk; completed = x.completed; iters = x.iters; rem = x.rem;
        nret += iters - 1u;
        if (x.refresh) { stop &= ~(EXT | IOX); HMA_REFRESH(); if (A->pending) stop |= HARD; }
        if (!completed || (fl & F_TF)) stop |= HARD;
        goto commit_c;
    }
    }

commit:
    completed = 1; anyc = 1;
commit_c:
    ip0 = nip;
    rem -= (int32_t)clk;
    nret++;
    if (__builtin_expect(inh, 0)) {
        if (completed) {
            if (c->irq_inhibit) c->irq_inhibit--;
            if (c->nmi_inhibit) c->nmi_inhibit--;
            if (c->debug_inhibit) c->debug_inhibit--;
        }
        inh = c->irq_inhibit | c->nmi_inhibit | c->debug_inhibit;
    }
#ifdef PC98_FAST16_WRITE_HOOK
    A->nfetch = n;
    memset(A->ib, 0, sizeof(A->ib));
    memcpy(A->ib, p, n < 8u ? n : 8u);
#endif
    if (__builtin_expect((rem <= 0) | (int)stop, 0)) {
        if (stop & EXT) {
            stop &= ~EXT;
            if (remclk) {
                /* a device saw the clock and may have retargeted it */
                rem = (int32_t)*remclk - off - (int32_t)clk;
                if (stop & IOX) { HMA_REFRESH(); stop &= ~IOX; if (A->pending) stop |= HARD; }
            } else stop |= HARD;
        }
        if (rem <= 0 || stop) goto finish;
    }
    if (__builtin_expect(!inh, 1)) goto mini_enter;
    goto next_insn;
mini_enter:
    {   /* run the hot opcodes as threaded handlers (fast16_mini.inc) until one of them hands an instruction back */
        const uint32_t cb = c->base[SG_CS];
        const uint32_t cw = M_CSWIN(cb);
        if (cw > ip0 && ip0 < 0xfff0u && M_TABOK(ram[cb + ip0])) {
            const uint8_t *const pcb = ram + cb;
            const uint32_t span = cw < 0xfff0u ? cw : 0xfff0u;
            A->mo_pcb = pcb; A->mo_off = off; A->mo_ev = 0; A->mo_rom = cb >= 0xe8000u;
            (void)M_START(A, pcb + ip0, rem, fl, pcb + span, 0, (const void *const *)mtab, ram);
            ip0 = A->mo_ip;
            rem = A->mo_rem; fl = A->mo_fl;
            if (A->mo_nret) { nret += A->mo_nret; anyc = 1; }
            if (A->mo_ev & MEV_HARD) stop |= HARD;
            inh = c->irq_inhibit | c->nmi_inhibit | c->debug_inhibit;
            if (rem <= 0 || stop) goto finish;
        }
    }
    goto next_insn;
finish:
    if (ext && anyc) A->s.flag_high &= (uint16_t)~1u;   /* RF clears after every completed instruction */
    c->ip = (uint16_t)ip0;
    c->flags = (uint16_t)fl;
    if (remclk) *remclk = rem + off;
    *clocks = (unsigned)(budget - rem);
    A->retired += nret;
    return 1;
bail:
    if (nret) goto finish;   /* earlier instructions of this batch stay executed; this one is untouched */
    return 0;
}

#undef mod
#undef reg
#undef rm
#undef F16_V30
#undef F16N3
#undef F16N2
#undef F16N
#undef h_misc
#undef h_ctl
#undef h_grp
#undef h_bcd
#undef h_str
#undef pc98_fast16_run_m
#undef H_LOAD
#undef H_SAVE
#undef H_END
#undef ALU16K
#undef ALU8K
#undef ALU16X
#undef ALU8X
#undef ALU_BODY
#undef ALU_ROW
#undef JCC
#undef SLT
#undef BAIL
#undef NEXT
#undef JUMP
