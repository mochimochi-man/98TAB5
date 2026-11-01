/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * fast16 mini loop, handlers that take a ModR/M memory operand (stage 14). Included twice by fast16_mini.inc:
 *   MINI_OV 0: names m_*  - default segment (DS, or SS for BP-based forms)
 *   MINI_OV 1: names mo_* - entered through a segment-override prefix handler (A->mo_ov holds the byte offset of the
 *              overriding segment base inside the adapter); a hand-back restores pc to the prefix byte. */
#undef MBAIL
#if MINI_OV
#define MN(x) mo_##x
#define MEA_BASE() (*(const uint32_t *)((const char *)A + A->mo_ov))
#define MBAIL() return m_exit(A, pc - 1, rem, fl, lim, nret, tab, ram)
#else
#define MN(x) m_##x
#define MEA_BASE() (*(const uint32_t *)((const char *)A + e_->bo))
#define MBAIL() return m_exit(MP)
#endif

#undef MEA
#define MEA(m) const EaEnt *const e_ = &g_eat[(m)]; \
    { const unsigned d16_ = (unsigned)pc[2] | ((unsigned)pc[3] << 8); \
      ea = ((unsigned)*(const uint16_t *)((const char *)A + e_->o1) + (unsigned)*(const uint16_t *)((const char *)A + e_->o2) + \
            ((d16_ & e_->mk16) | ((unsigned)(int)(int8_t)pc[2] & e_->mk8))) & 0xffffu; \
      lin = MEA_BASE() + ea; nb = 2u + e_->dlen; }

/* ---- ALU r/m,r  r,r/m ---- */
#define M_ALU_RM(OPC, AOP, FORM) MH(MN(OPC)) { \
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned res, a, b; \
    if ((FORM) & 1u) { \
        if ((FORM) & 2u || (AOP) == 7) M_CHK_R16(); else M_CHK_RW16(); \
        a = (FORM) & 2u ? r[reg] : (mem ? M_RD16(lin) : r[rm]); b = (FORM) & 2u ? (mem ? M_RD16(lin) : r[rm]) : r[reg]; \
        M_ALU16(AOP, a, b, res); \
        if ((AOP) != 7) { if ((FORM) & 2u) r[reg] = (uint16_t)res; else if (mem) M_WR16(lin, res); else r[rm] = (uint16_t)res; } \
    } else { \
        if ((FORM) & 2u || (AOP) == 7) M_CHK_R8(); else M_CHK_RW8(); \
        a = (FORM) & 2u ? GET8(reg) : (mem ? (unsigned)ram[lin] : GET8(rm)); b = (FORM) & 2u ? (mem ? (unsigned)ram[lin] : GET8(rm)) : GET8(reg); \
        M_ALU8(AOP, a, b, res); \
        if ((AOP) != 7) { if ((FORM) & 2u) SET8(reg, res); else if (mem) M_WR8(lin, res); else SET8(rm, res); } \
    } \
    MNEXT(nb, MCKM(OPC, mem)); }
#define M_ALU_ROW4(o0, o1, o2, o3, AOP) M_ALU_RM(o0, AOP, 0) M_ALU_RM(o1, AOP, 1) M_ALU_RM(o2, AOP, 2) M_ALU_RM(o3, AOP, 3)
M_ALU_ROW4(0x00, 0x01, 0x02, 0x03, 0) M_ALU_ROW4(0x08, 0x09, 0x0a, 0x0b, 1) M_ALU_ROW4(0x10, 0x11, 0x12, 0x13, 2) M_ALU_ROW4(0x18, 0x19, 0x1a, 0x1b, 3)
M_ALU_ROW4(0x20, 0x21, 0x22, 0x23, 4) M_ALU_ROW4(0x28, 0x29, 0x2a, 0x2b, 5) M_ALU_ROW4(0x30, 0x31, 0x32, 0x33, 6) M_ALU_ROW4(0x38, 0x39, 0x3a, 0x3b, 7)

/* ---- 80-83: ALU r/m, imm ---- */
MH(MN(grp1_b))   /* 80 82 */
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned res, a, b = pc[nb];
    const unsigned op = pc[0];
    M_CHK_RW8();
    a = mem ? (unsigned)ram[lin] : GET8(rm);
    switch (reg) {
    case 0: res = f_add8(a, b, 0, &fl); break;
    case 1: res = f_log8(a | b, &fl); break;
    case 2: res = f_add8(a, b, fl & F_CF, &fl); break;
    case 3: res = f_sub8(a, b, fl & F_CF, &fl); break;
    case 4: res = f_log8(a & b, &fl); break;
    case 5: res = f_sub8(a, b, 0, &fl); break;
    case 6: res = f_log8(a ^ b, &fl); break;
    default: res = f_sub8(a, b, 0, &fl); break;
    }
    if (reg != 7) { if (mem) M_WR8(lin, res); else SET8(rm, res); }
    MNEXT(nb + 1, (unsigned)A->mck_main[((unsigned)op << 4) | (reg << 1) | mem]);   /* by operation: CMP is shorter */
}
MH(MN(grp1_w))   /* 81 83 */
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned res, a, b;
    const unsigned op = pc[0];
    M_CHK_RW16();
    if (op == 0x83) { b = (unsigned)(int)(int8_t)pc[nb] & 0xffffu; nb += 1; } else { b = (unsigned)pc[nb] | ((unsigned)pc[nb + 1] << 8); nb += 2; }
    a = mem ? M_RD16(lin) : r[rm];
    switch (reg) {
    case 0: res = f_add16(a, b, 0, &fl); break;
    case 1: res = f_log16(a | b, &fl); break;
    case 2: res = f_add16(a, b, fl & F_CF, &fl); break;
    case 3: res = f_sub16(a, b, fl & F_CF, &fl); break;
    case 4: res = f_log16(a & b, &fl); break;
    case 5: res = f_sub16(a, b, 0, &fl); break;
    case 6: res = f_log16(a ^ b, &fl); break;
    default: res = f_sub16(a, b, 0, &fl); break;
    }
    if (reg != 7) { if (mem) M_WR16(lin, res); else r[rm] = (uint16_t)res; }
    MNEXT(nb, (unsigned)A->mck_main[((unsigned)op << 4) | (reg << 1) | mem]);
}

/* ---- TEST ---- */
MH(MN(84)) { uint16_t *const r = A->s.low.r; MMODRM(); M_CHK_R8(); (void)f_log8((mem ? (unsigned)ram[lin] : GET8(rm)) & GET8(reg), &fl); MNEXT(nb, MCKM(0x84, mem)); }
MH(MN(85)) { uint16_t *const r = A->s.low.r; MMODRM(); M_CHK_R16(); (void)f_log16((mem ? M_RD16(lin) : r[rm]) & r[reg], &fl); MNEXT(nb, MCKM(0x85, mem)); }

/* ---- MOV ---- */
MH(MN(88)) { uint16_t *const r = A->s.low.r; MMODRM();
    if (mem && lin >= 0xa0000u) { if (!M_GVRAM(lin)) MBAIL(); M_GVWR8(lin, GET8(reg)); MNEXT(nb, MCKM(0x88, mem)); }
    if (mem) M_WR8(lin, GET8(reg)); else SET8(rm, GET8(reg)); MNEXT(nb, MCKM(0x88, mem)); }
MH(MN(89)) { uint16_t *const r = A->s.low.r; MMODRM(); M_CHK_RW16(); if (mem) M_WR16(lin, r[reg]); else r[rm] = r[reg]; MNEXT(nb, MCKM(0x89, mem)); }
MH(MN(8a)) { uint16_t *const r = A->s.low.r; MMODRM();
    if (mem && lin >= 0xa4000u) { if (!M_GVRAM(lin)) MBAIL(); SET8(reg, M_GVRD8(lin)); MNEXT(nb, MCKM(0x8a, mem)); }
    { const unsigned v = mem ? (unsigned)ram[lin] : GET8(rm); SET8(reg, v); } MNEXT(nb, MCKM(0x8a, mem)); }
MH(MN(8b)) { uint16_t *const r = A->s.low.r; MMODRM(); M_CHK_R16(); { const unsigned v = mem ? M_RD16(lin) : r[rm]; r[reg] = (uint16_t)v; } MNEXT(nb, MCKM(0x8b, mem)); }
MH(MN(8c))   /* MOV r/m16, sreg */
{
    uint16_t *const r = A->s.low.r; MMODRM();
    if (reg > 3u) MBAIL();
    M_CHK_RW16();
    if (mem) M_WR16(lin, A->s.low.seg[reg]); else r[rm] = A->s.low.seg[reg];
    MNEXT(nb, MCKM(0x8c, mem));
}
MH(MN(8e))   /* MOV sreg, r/m16 (not CS, not SS) */
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v;
    if (reg > 3u || reg == SG_CS || reg == SG_SS) MBAIL();
    M_CHK_R16();
    v = mem ? M_RD16(lin) : r[rm];
    M_LOADSEG(reg, v);
    MNEXT(nb, (unsigned)A->mck_main[(0x8eu << 4) | (reg << 1) | mem]);
}
MH(MN(c6))
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v;
    if (reg) MBAIL();
    M_CHK_RW8();
    v = pc[nb];
    if (mem) M_WR8(lin, v); else SET8(rm, v);
    MNEXT(nb + 1, MCKM(0xc6, mem));
}
MH(MN(c7))
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v;
    if (reg) MBAIL();
    M_CHK_RW16();
    v = (unsigned)pc[nb] | ((unsigned)pc[nb + 1] << 8);
    if (mem) M_WR16(lin, v); else r[rm] = (uint16_t)v;
    MNEXT(nb + 2, MCKM(0xc7, mem));
}
MH(MN(mova))   /* A0-A3: accumulator <-> [moffs] */
{
    uint16_t *const r = A->s.low.r; const unsigned op = pc[0], off = (unsigned)pc[1] | ((unsigned)pc[2] << 8);
#if MINI_OV
    const uint32_t a = *(const uint32_t *)((const char *)A + A->mo_ov) + off;
#else
    const uint32_t a = A->s.low.base[SG_DS] + off;
#endif
    switch (op) {
    case 0xa0: if (a >= 0xa4000u) MBAIL(); SET8(0, ram[a]); break;
    case 0xa1: if (off == 0xffffu || a >= 0xa3fffu) MBAIL(); r[0] = (uint16_t)M_RD16(a); break;
    case 0xa2: if (a >= 0xa0000u) MBAIL(); M_WR8(a, r[0]); break;
    default:   if (off == 0xffffu || a >= 0x9ffffu) MBAIL(); M_WR16(a, r[0]); break;
    }
    MNEXT(3, MCKN(op));
}

/* ---- FE / FF / F6 / F7 ---- */
MH(MN(fe))
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v, res;
    if (reg > 1) MBAIL();
    M_CHK_RW8();
    v = mem ? (unsigned)ram[lin] : GET8(rm);
    res = reg ? f_dec8(v, &fl) : f_inc8(v, &fl);
    if (mem) M_WR8(lin, res); else SET8(rm, res);
    MNEXT(nb, MCKM(0xfe, mem));
}
MH(MN(ff))
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v;
    const unsigned clk = A->mck_main[(0xffu << 4) | (reg << 1) | mem];
    if (reg == 3 || reg == 5 || reg == 7) MBAIL();
    if (reg <= 1) M_CHK_RW16(); else M_CHK_R16();
    v = mem ? M_RD16(lin) : r[rm];
    switch (reg) {
    case 0: v = f_inc16(v, &fl); if (mem) M_WR16(lin, v); else r[rm] = (uint16_t)v; MNEXT(nb, clk);
    case 1: v = f_dec16(v, &fl); if (mem) M_WR16(lin, v); else r[rm] = (uint16_t)v; MNEXT(nb, clk);
    case 2: {
        const unsigned sp = r[4], ret = (MIP() + nb) & 0xffffu, nsp = (sp - 2u) & 0xffffu;
        const uint32_t a = A->s.low.base[SG_SS] + nsp;
        if (sp == 1u || a >= 0x9ffffu) MBAIL();
        M_WR16(a, ret); r[4] = (uint16_t)nsp;
        MGO(A->mo_pcb + v, clk);
    }
    case 4: MGO(A->mo_pcb + v, clk);
    default: {   /* 6 */
        const unsigned sp = r[4], nsp = (sp - 2u) & 0xffffu;
        const uint32_t a = A->s.low.base[SG_SS] + nsp;
        if (sp == 1u || a >= 0x9ffffu) MBAIL();
        M_WR16(a, v); r[4] = (uint16_t)nsp;
        MNEXT(nb, clk);
    }
    }
}
MH(MN(f6))   /* TEST NOT NEG on r/m8 */
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v, res, imm = 0;
    const unsigned clk = A->mck_main[(0xf6u << 4) | (reg << 1) | mem];
    if (reg == 1 || reg >= 4) MBAIL();
    if (reg == 0) M_CHK_R8(); else M_CHK_RW8();
    v = mem ? (unsigned)ram[lin] : GET8(rm);
    if (reg == 0) { imm = pc[nb]; (void)f_log8(v & imm, &fl); MNEXT(nb + 1, clk); }
    res = reg == 2 ? ~v : f_sub8(0, v, 0, &fl);
    if (mem) M_WR8(lin, res); else SET8(rm, res);
    MNEXT(nb, clk);
}
MH(MN(f7))   /* TEST NOT NEG on r/m16 */
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v, res, imm = 0;
    const unsigned clk = A->mck_main[(0xf7u << 4) | (reg << 1) | mem];
    if (reg == 1 || reg >= 4) MBAIL();
    if (reg == 0) M_CHK_R16(); else M_CHK_RW16();
    v = mem ? M_RD16(lin) : r[rm];
    if (reg == 0) { imm = (unsigned)pc[nb] | ((unsigned)pc[nb + 1] << 8); (void)f_log16(v & imm, &fl); MNEXT(nb + 2, clk); }
    res = reg == 2 ? ~v : f_sub16(0, v, 0, &fl);
    if (mem) M_WR16(lin, res); else r[rm] = (uint16_t)res;
    MNEXT(nb, clk);
}

/* ---- shifts / rotates: by 1 (D0 D1), by CL (D2 D3), by imm8 (C0 C1) ---- */
MH(MN(d0))
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v, res;
    if (reg == 6) MBAIL();
    M_CHK_RW8();
    v = mem ? (unsigned)ram[lin] : GET8(rm);
    switch (reg) {
    case 0: res = f_shift1(0, 8, v, &fl); break; case 1: res = f_shift1(1, 8, v, &fl); break;
    case 2: res = f_shift1(2, 8, v, &fl); break; case 3: res = f_shift1(3, 8, v, &fl); break;
    case 4: res = f_shift1(4, 8, v, &fl); break; case 5: res = f_shift1(5, 8, v, &fl); break;
    default: res = f_shift1(7, 8, v, &fl); break;
    }
    if (mem) M_WR8(lin, res); else SET8(rm, res);
    MNEXT(nb, MCKM(0xd0, mem));
}
MH(MN(d1))
{
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned v, res;
    if (reg == 6) MBAIL();
    M_CHK_RW16();
    v = mem ? M_RD16(lin) : r[rm];
    switch (reg) {
    case 0: res = f_shift1(0, 16, v, &fl); break; case 1: res = f_shift1(1, 16, v, &fl); break;
    case 2: res = f_shift1(2, 16, v, &fl); break; case 3: res = f_shift1(3, 16, v, &fl); break;
    case 4: res = f_shift1(4, 16, v, &fl); break; case 5: res = f_shift1(5, 16, v, &fl); break;
    default: res = f_shift1(7, 16, v, &fl); break;
    }
    if (mem) M_WR16(lin, res); else r[rm] = (uint16_t)res;
    MNEXT(nb, MCKM(0xd1, mem));
}
/* C0 C1 D2 D3: count handling and clocks exactly as the generic loop */
#define M_SHIFTN(NAME, OPC, WORD) MH(MN(NAME)) { \
    uint16_t *const r = A->s.low.r; MMODRM(); unsigned cnt, extra, v, res; \
    const unsigned v30 = A->menu == 0; \
    if (reg == 6) MBAIL(); \
    if ((OPC) < 0xd0u) { cnt = pc[nb]; extra = (unsigned)A->mck_shift[(OPC) & 1u] * (cnt & 31u); nb += 1; } \
    else { cnt = r[1] & 255u; extra = (unsigned)A->mck_shift[2u + ((OPC) & 1u)] * (cnt & 31u); } \
    if (!v30) cnt &= 31u; \
    if (!cnt) { if (!v30 && mem) MBAIL(); MNEXT(nb, MCKM(OPC, mem) + extra); } \
    if (WORD) { M_CHK_RW16(); v = mem ? M_RD16(lin) : r[rm]; res = f_shift(reg, 16, v, cnt, &fl); \
        if (mem) M_WR16(lin, res); else r[rm] = (uint16_t)res; } \
    else { M_CHK_RW8(); v = mem ? (unsigned)ram[lin] : GET8(rm); res = f_shift(reg, 8, v, cnt, &fl); \
        if (mem) M_WR8(lin, res); else SET8(rm, res); } \
    MNEXT(nb, MCKM(OPC, mem) + extra); }
M_SHIFTN(c0, 0xc0u, 0) M_SHIFTN(c1, 0xc1u, 1) M_SHIFTN(d2, 0xd2u, 0) M_SHIFTN(d3, 0xd3u, 1)

static void MN(register)(mh_t *t)
{
    unsigned i;
    for (i = 0; i < 0x40; i += 8) {
        static mh_t const alu[8][4] = { { MN(0x00), MN(0x01), MN(0x02), MN(0x03) }, { MN(0x08), MN(0x09), MN(0x0a), MN(0x0b) },
            { MN(0x10), MN(0x11), MN(0x12), MN(0x13) }, { MN(0x18), MN(0x19), MN(0x1a), MN(0x1b) }, { MN(0x20), MN(0x21), MN(0x22), MN(0x23) },
            { MN(0x28), MN(0x29), MN(0x2a), MN(0x2b) }, { MN(0x30), MN(0x31), MN(0x32), MN(0x33) }, { MN(0x38), MN(0x39), MN(0x3a), MN(0x3b) } };
        unsigned k;
        for (k = 0; k < 4; k++) t[i + k] = alu[i >> 3][k];
    }
    t[0x80] = t[0x82] = MN(grp1_b); t[0x81] = t[0x83] = MN(grp1_w);
    t[0x84] = MN(84); t[0x85] = MN(85);
    t[0x88] = MN(88); t[0x89] = MN(89); t[0x8a] = MN(8a); t[0x8b] = MN(8b);
    for (i = 0xa0; i < 0xa4; i++) t[i] = MN(mova);
    t[0xc6] = MN(c6); t[0xc7] = MN(c7);
#if !MINI_OV   /* the override variants of the rarer opcodes are left out (IRAM budget: they go to the generic loop) */
    t[0x8c] = MN(8c); t[0x8e] = MN(8e);
    t[0xc0] = MN(c0); t[0xc1] = MN(c1);
    t[0xd0] = MN(d0); t[0xd1] = MN(d1); t[0xd2] = MN(d2); t[0xd3] = MN(d3);
    t[0xf6] = MN(f6); t[0xf7] = MN(f7); t[0xfe] = MN(fe); t[0xff] = MN(ff);
#endif
}
#undef MN
#undef MEA_BASE
#undef MBAIL
#undef M_ALU_RM
#undef M_ALU_ROW4
#undef M_SHIFTN
