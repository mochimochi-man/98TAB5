/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pmfast.inc (included by cpuadapt.c) - 286 protected mode, the common instructions without the reference step.
 *
 * The reference interpreter (execute.c) takes about 1,300 host cycles an instruction through its general resolver
 * bus; a program that keeps going into protected mode to move data to and from extended memory spent most of its
 * time there and the machine fell below real time (the music played during it broke up). This takes the plain
 * instructions such code is made of - moves, arithmetic, near branches, the stack, string moves - and does them
 * directly, with the reference's own ALU / shifter (pc98_cpu_alu, pc98_cpu_shift) for the flags and its own
 * descriptor check (pc98_cpu_translate286) for every access, the code fetch and branch targets included. Every
 * check is made before anything is changed: an instruction that would fault, or any other instruction, is left
 * untouched to the reference step, which raises the fault or runs it. Clocks come from the same classifier
 * (clocks_of) the reference step uses.
 */

/* pc98_cpu_translate286 + pc98_cpu_range286 (protected286.c, descriptor286.c), the same tests in the same order */
static inline __attribute__((always_inline)) int pmf_ok(const Pc98ProtectedSegments286 *P, unsigned seg, unsigned off, unsigned size, enum Pc98SegmentAccess286 acc, uint32_t *phys)
{
    const Pc98Descriptor286 *d = &P->cache[seg];
    const unsigned ar = d->access;

    if (!P->valid[seg] || !(ar & 16)) return 0;
    if (acc == PC98_SEG_EXECUTE) { if (!(ar & 8) && !(seg == PC98_CS && (ar & 0x1eu) == 0x12u)) return 0; }
    else if (acc == PC98_SEG_WRITE) { if ((ar & 8) || !(ar & 2)) return 0; }
    else if ((ar & 8) && !(ar & 2)) return 0;
    if (!size || size > 0x10000u - off) return 0;
    if ((ar & 0x1cu) == 0x14u) { if (!(off > d->limit)) return 0; }
    else if (off + size - 1 > d->limit) return 0;
    *phys = (d->base + off) & 0xffffffu;
    return 1;
}

static inline unsigned pmf_reg(const Pc98Cpu16State *c, unsigned r, unsigned bits)
{
    return bits == 16 ? c->r[r] : (c->r[r & 3] >> (r < 4 ? 0 : 8)) & 255u;
}

static inline void pmf_setreg(Pc98Cpu16State *c, unsigned r, unsigned bits, unsigned v)
{
    if (bits == 16) c->r[r] = (uint16_t)v;
    else {
        const unsigned shift = r < 4 ? 0 : 8, i = r & 3;
        c->r[i] = (uint16_t)((c->r[i] & ~(255u << shift)) | ((v & 255u) << shift));
    }
}

static inline unsigned pmf_alu_wrap(unsigned op, unsigned bits, unsigned l, unsigned r, uint16_t *fl)
{
    return pc98_cpu_alu((enum Pc98AluOp)op, bits, (uint16_t)l, (uint16_t)r, fl);
}

/* main memory and extended memory directly (as bus.c reaches them), anything else through the bus */
static inline __attribute__((always_inline)) uint8_t *pmf_ram(uint32_t a, unsigned n)
{
    if (a + n <= 0xa0000u) return mem + a;
    if ((g_cpu.addr_mask & 0x100000u) && a >= HIMEM_END && a + n <= g_cpu.ext_end16) return g_cpu.ext + (a - 0x100000u);
    return 0;
}
static inline __attribute__((always_inline)) unsigned pmf_rd(uint32_t a, unsigned bits)
{
    const uint8_t *p = pmf_ram(a, bits / 8);

    if (p) return bits == 8 ? p[0] : (unsigned)(p[0] | (p[1] << 8));
    return bits == 8 ? rd8(0, a) : rd16(0, a);
}
static inline __attribute__((always_inline)) void pmf_wr(uint32_t a, unsigned bits, unsigned v)
{
    uint8_t *p = pmf_ram(a, bits / 8);

    if (p) { p[0] = (uint8_t)v; if (bits == 16) p[1] = (uint8_t)(v >> 8); return; }
    if (bits == 8) wr8(0, a, (uint8_t)v); else wr16(0, a, (uint16_t)v);
}

typedef struct { unsigned is_reg, rm, reg, seg; uint16_t off; } PmfOp;



/* ModR/M at ib[*pos]; seg = override or -1 */
static inline __attribute__((always_inline)) void pmf_modrm(const Pc98Cpu16State *c, const uint8_t *ib, unsigned *pos, int seg, PmfOp *o)
{
    const unsigned m = ib[(*pos)++], mod = m >> 6, rm = m & 7;
    unsigned off = 0, dseg = PC98_DS;

    o->reg = (m >> 3) & 7;
    o->rm = rm;
    o->is_reg = mod == 3;
    if (mod == 3) { o->seg = 0; o->off = 0; return; }
    switch (rm) {
    case 0: off = c->r[PC98_BX] + c->r[PC98_SI]; break;
    case 1: off = c->r[PC98_BX] + c->r[PC98_DI]; break;
    case 2: off = c->r[PC98_BP] + c->r[PC98_SI]; dseg = PC98_SS; break;
    case 3: off = c->r[PC98_BP] + c->r[PC98_DI]; dseg = PC98_SS; break;
    case 4: off = c->r[PC98_SI]; break;
    case 5: off = c->r[PC98_DI]; break;
    case 6:
        if (mod == 0) { off = ib[*pos] | (ib[*pos + 1] << 8); *pos += 2; }
        else { off = c->r[PC98_BP]; dseg = PC98_SS; }
        break;
    default: off = c->r[PC98_BX]; break;
    }
    if (mod == 1) { off += (unsigned)(int)(int8_t)ib[*pos]; *pos += 1; }
    else if (mod == 2) { off += ib[*pos] | (ib[*pos + 1] << 8); *pos += 2; }
    o->off = (uint16_t)off;
    o->seg = seg >= 0 ? (unsigned)seg : dseg;
}

static inline int pmf_cond(unsigned code, unsigned fl)
{
    const unsigned of = (fl & PC98_OF) != 0, cf = (fl & PC98_CF) != 0, zf = (fl & PC98_ZF) != 0;
    const unsigned sf = (fl & PC98_SF) != 0, pf = (fl & PC98_PF) != 0;
    unsigned r;

    switch (code >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf | zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf ^ of; break;
    default: r = (sf ^ of) | zf; break;
    }
    return (int)(r ^ (code & 1));
}

/* Returns 1 when the instruction ran (clocks in *clk), 0 when the reference step must take it (nothing changed).
 * Kept out of line and in flash (linker.lf): inlined into the IRAM loop it cost 12KB of internal RAM. */
static __attribute__((noinline, noclone)) int pm_fast_step(Pc98Adapter *A, unsigned *clk)
{
    Pc98Cpu16State *c = &A->s.low;
    const Pc98ProtectedSegments286 *P = &A->mode.segments;
    const Pc98Descriptor286 *cs = &P->cache[PC98_CS];
    const unsigned ip0 = c->ip, cx0 = c->r[PC98_CX], cl0 = c->r[PC98_CX] & 255u;
    uint8_t ib[12];
    uint32_t lin, pa = 0, pb = 0;
    unsigned pos = 0, op, bits, rep = 0, completed = 1, next, target;
    int seg = -1;
    PmfOp o;

    if (c->model != PC98_CPU_286 || c->fault.valid || c->halted || (c->flags & PC98_TF) || A->mode.shutdown) return 0;
    if (!P->valid[PC98_CS] || ip0 > 0xfff0u) return 0;
    /* the code bytes are read ahead of the check: only from plain memory, where a read has no effect - RAM, or the
     * BIOS ROM (E8000h-FFFFFh, kept in mem[] as bus.c reads it; not while the ITF bank is in) */
    lin = (cs->base + ip0) & 0xffffffu;
    {
        const uint8_t *code = pmf_ram(lin, 12);
        if (!code && lin >= 0xe8000u && lin + 12 <= 0x100000u && !g_cpu.itf_rom && (g_cpu.addr_mask & 0x100000u)) code = mem + lin;
        if (!code) return 0;
        memcpy(ib, code, 12);
    }

    for (;;) {
        op = ib[pos];
        if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e) seg = (int)((op >> 3) & 3);
        else if (op == 0xf3) rep = op;
        else break;
        if (++pos > 3) return 0;
    }
    pos++;
    bits = (op & 1) ? 16 : 8;

    /* ---- decode and check; nothing is changed until "commit" ---- */
    enum { K_ALU_RM, K_ALU_R, K_ALU_AI, K_GRP1, K_TEST_RM, K_TEST_AI, K_MOV_RMR, K_MOV_RRM, K_MOV_RMI, K_MOV_RI,
           K_MOV_AM, K_MOV_MA, K_XCHG_RM, K_XCHG_AX, K_INCDEC_R, K_INCDEC_RM, K_PUSH, K_POP, K_LEA, K_JCC, K_LOOP,
           K_JMP, K_CALL, K_RET, K_CBW, K_CWD, K_FLAG, K_SHIFT, K_STR, K_NOP } kind;
    unsigned aop = 0, imm = 0, cnt = 0;

    if (rep && !(op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab || op == 0xac || op == 0xad)) return 0;
    if (op < 0x40 && (op & 7) < 6) {
        aop = op >> 3;
        switch (op & 7) {
        case 0: case 1: kind = K_ALU_RM; bits = (op & 1) ? 16 : 8; pmf_modrm(c, ib, &pos, seg, &o); break;
        case 2: case 3: kind = K_ALU_R; bits = (op & 1) ? 16 : 8; pmf_modrm(c, ib, &pos, seg, &o); break;
        case 4: kind = K_ALU_AI; bits = 8; imm = ib[pos++]; break;
        default: kind = K_ALU_AI; bits = 16; imm = ib[pos] | (ib[pos + 1] << 8); pos += 2; break;
        }
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        kind = K_GRP1; bits = op == 0x80 ? 8 : 16; pmf_modrm(c, ib, &pos, seg, &o); aop = o.reg;
        if (op == 0x81) { imm = ib[pos] | (ib[pos + 1] << 8); pos += 2; }
        else if (op == 0x83) imm = (unsigned)(uint16_t)(int16_t)(int8_t)ib[pos++];
        else imm = ib[pos++];
    } else if (op == 0x84 || op == 0x85) { kind = K_TEST_RM; pmf_modrm(c, ib, &pos, seg, &o); }
    else if (op == 0xa8) { kind = K_TEST_AI; bits = 8; imm = ib[pos++]; }
    else if (op == 0xa9) { kind = K_TEST_AI; bits = 16; imm = ib[pos] | (ib[pos + 1] << 8); pos += 2; }
    else if (op == 0x88 || op == 0x89) { kind = K_MOV_RMR; pmf_modrm(c, ib, &pos, seg, &o); }
    else if (op == 0x8a || op == 0x8b) { kind = K_MOV_RRM; pmf_modrm(c, ib, &pos, seg, &o); }
    else if (op == 0xc6 || op == 0xc7) {
        kind = K_MOV_RMI; pmf_modrm(c, ib, &pos, seg, &o);
        if (o.reg) return 0;
        if (bits == 16) { imm = ib[pos] | (ib[pos + 1] << 8); pos += 2; } else imm = ib[pos++];
    } else if (op >= 0xb0 && op <= 0xbf) {
        kind = K_MOV_RI; bits = op < 0xb8 ? 8 : 16;
        if (bits == 16) { imm = ib[pos] | (ib[pos + 1] << 8); pos += 2; } else imm = ib[pos++];
    } else if (op >= 0xa0 && op <= 0xa3) {
        kind = op < 0xa2 ? K_MOV_AM : K_MOV_MA;
        o.is_reg = 0; o.seg = seg >= 0 ? (unsigned)seg : PC98_DS; o.off = (uint16_t)(ib[pos] | (ib[pos + 1] << 8)); pos += 2;
    } else if (op == 0x86 || op == 0x87) { kind = K_XCHG_RM; pmf_modrm(c, ib, &pos, seg, &o); }
    else if (op == 0x90) kind = K_NOP;
    else if (op > 0x90 && op <= 0x97) { kind = K_XCHG_AX; bits = 16; }
    else if (op >= 0x40 && op <= 0x4f) { kind = K_INCDEC_R; bits = 16; }
    else if (op == 0xfe || op == 0xff) {
        kind = K_INCDEC_RM; pmf_modrm(c, ib, &pos, seg, &o);
        if (o.reg > 1) return 0;
    } else if (op >= 0x50 && op <= 0x5f) { kind = op < 0x58 ? K_PUSH : K_POP; bits = 16; }
    else if (op == 0x8d) { kind = K_LEA; bits = 16; pmf_modrm(c, ib, &pos, seg, &o); if (o.is_reg) return 0; }
    else if (op >= 0x70 && op <= 0x7f) { kind = K_JCC; imm = (unsigned)(uint16_t)(int16_t)(int8_t)ib[pos++]; }
    else if (op >= 0xe0 && op <= 0xe3) { kind = K_LOOP; imm = (unsigned)(uint16_t)(int16_t)(int8_t)ib[pos++]; }
    else if (op == 0xeb) { kind = K_JMP; imm = (unsigned)(uint16_t)(int16_t)(int8_t)ib[pos++]; }
    else if (op == 0xe9 || op == 0xe8) { kind = op == 0xe9 ? K_JMP : K_CALL; imm = ib[pos] | (ib[pos + 1] << 8); pos += 2; }
    else if (op == 0xc3) kind = K_RET;
    else if (op == 0x98) kind = K_CBW;
    else if (op == 0x99) kind = K_CWD;
    else if (op == 0xf5 || op == 0xf8 || op == 0xf9 || op == 0xfc || op == 0xfd) kind = K_FLAG;
    else if (op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3)) {
        kind = K_SHIFT; pmf_modrm(c, ib, &pos, seg, &o);
        if (o.reg == 6) return 0;
        cnt = op < 0xd0 ? ib[pos++] : op < 0xd2 ? 1u : (c->r[PC98_CX] & 255u);
        cnt &= 31;
        if (!cnt) return 0;
    } else if (op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab || op == 0xac || op == 0xad) kind = K_STR;
    else return 0;

    if (pos > 10) return 0;
    next = (ip0 + pos) & 0xffffu;
    if (next < ip0) return 0;   /* the instruction would run past FFFFh */
    if (!pmf_ok(P, PC98_CS, ip0, pos, PC98_SEG_EXECUTE, &pa)) return 0;

    /* memory operand: access check now (write access for anything that writes it) */
    {
        int needs_mem = 0;
        enum Pc98SegmentAccess286 acc = PC98_SEG_READ;

        switch (kind) {
        case K_ALU_RM: case K_GRP1: needs_mem = !o.is_reg; acc = PC98_SEG_WRITE; break;   /* CMP too: where the reference might only read, falling back is the safe side */
        case K_ALU_R: case K_TEST_RM: case K_MOV_RRM: case K_MOV_AM: needs_mem = !o.is_reg; break;
        case K_MOV_RMR: case K_MOV_RMI: case K_MOV_MA: case K_XCHG_RM: case K_INCDEC_RM: case K_SHIFT:
            needs_mem = !o.is_reg; acc = PC98_SEG_WRITE; break;
        default: break;
        }
        if (needs_mem && !pmf_ok(P, o.seg, o.off, bits / 8, acc, &pa)) return 0;
    }

    /* ---- commit ---- */
    switch (kind) {
    case K_ALU_RM: {
        const unsigned l = o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits);
        const unsigned r = pmf_alu_wrap(aop, bits, l, pmf_reg(c, o.reg, bits), &c->flags);
        if (aop != PC98_CMP) { if (o.is_reg) pmf_setreg(c, o.rm, bits, r); else pmf_wr(pa, bits, r); }
        c->ip = (uint16_t)next;
        break;
    }
    case K_ALU_R: {
        const unsigned rv = o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits);
        const unsigned r = pmf_alu_wrap(aop, bits, pmf_reg(c, o.reg, bits), rv, &c->flags);
        if (aop != PC98_CMP) pmf_setreg(c, o.reg, bits, r);
        c->ip = (uint16_t)next;
        break;
    }
    case K_ALU_AI: {
        const unsigned r = pmf_alu_wrap(aop, bits, pmf_reg(c, PC98_AX, bits), imm, &c->flags);
        if (aop != PC98_CMP) pmf_setreg(c, PC98_AX, bits, r);
        c->ip = (uint16_t)next;
        break;
    }
    case K_GRP1: {
        const unsigned l = o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits);
        const unsigned r = pmf_alu_wrap(aop, bits, l, imm, &c->flags);
        if (aop != PC98_CMP) { if (o.is_reg) pmf_setreg(c, o.rm, bits, r); else pmf_wr(pa, bits, r); }
        c->ip = (uint16_t)next;
        break;
    }
    case K_TEST_RM: {
        const unsigned l = o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits);
        (void)pc98_cpu_alu(PC98_AND, bits, (uint16_t)l, (uint16_t)pmf_reg(c, o.reg, bits), &c->flags);
        c->ip = (uint16_t)next;
        break;
    }
    case K_TEST_AI:
        (void)pc98_cpu_alu(PC98_AND, bits, (uint16_t)pmf_reg(c, PC98_AX, bits), (uint16_t)imm, &c->flags);
        c->ip = (uint16_t)next;
        break;
    case K_MOV_RMR:
        if (o.is_reg) pmf_setreg(c, o.rm, bits, pmf_reg(c, o.reg, bits)); else pmf_wr(pa, bits, pmf_reg(c, o.reg, bits));
        c->ip = (uint16_t)next;
        break;
    case K_MOV_RRM:
        pmf_setreg(c, o.reg, bits, o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits));
        c->ip = (uint16_t)next;
        break;
    case K_MOV_RMI:
        if (o.is_reg) pmf_setreg(c, o.rm, bits, imm); else pmf_wr(pa, bits, imm);
        c->ip = (uint16_t)next;
        break;
    case K_MOV_RI:
        pmf_setreg(c, op & 7, bits, imm);
        c->ip = (uint16_t)next;
        break;
    case K_MOV_AM:
        pmf_setreg(c, PC98_AX, bits, pmf_rd(pa, bits));
        c->ip = (uint16_t)next;
        break;
    case K_MOV_MA:
        pmf_wr(pa, bits, pmf_reg(c, PC98_AX, bits));
        c->ip = (uint16_t)next;
        break;
    case K_XCHG_RM: {
        const unsigned a = pmf_reg(c, o.reg, bits);
        if (o.is_reg) { pmf_setreg(c, o.reg, bits, pmf_reg(c, o.rm, bits)); pmf_setreg(c, o.rm, bits, a); }
        else { pmf_setreg(c, o.reg, bits, pmf_rd(pa, bits)); pmf_wr(pa, bits, a); }
        c->ip = (uint16_t)next;
        break;
    }
    case K_XCHG_AX: {
        const uint16_t a = c->r[PC98_AX];
        c->r[PC98_AX] = c->r[op & 7]; c->r[op & 7] = a;
        c->ip = (uint16_t)next;
        break;
    }
    case K_NOP:
        c->ip = (uint16_t)next;
        break;
    case K_INCDEC_R:
        c->r[op & 7] = pc98_cpu_alu(op < 0x48 ? PC98_INC : PC98_DEC, 16, c->r[op & 7], 1, &c->flags);
        c->ip = (uint16_t)next;
        break;
    case K_INCDEC_RM: {
        const unsigned l = o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits);
        const unsigned r = pc98_cpu_alu(o.reg ? PC98_DEC : PC98_INC, bits, (uint16_t)l, 1, &c->flags);
        if (o.is_reg) pmf_setreg(c, o.rm, bits, r); else pmf_wr(pa, bits, r);
        c->ip = (uint16_t)next;
        break;
    }
    case K_PUSH: {
        const uint16_t sp = (uint16_t)(c->r[PC98_SP] - 2);
        const uint16_t v = c->r[op & 7];
        if (!pmf_ok(P, PC98_SS, sp, 2, PC98_SEG_WRITE, &pa)) return 0;
        pmf_wr(pa, 16, v);
        c->r[PC98_SP] = sp;
        c->ip = (uint16_t)next;
        break;
    }
    case K_POP: {
        if (!pmf_ok(P, PC98_SS, c->r[PC98_SP], 2, PC98_SEG_READ, &pa)) return 0;
        {
            const uint16_t v = (uint16_t)pmf_rd(pa, 16);
            c->r[PC98_SP] = (uint16_t)(c->r[PC98_SP] + 2);
            c->r[op & 7] = v;
        }
        c->ip = (uint16_t)next;
        break;
    }
    case K_LEA:
        c->r[o.reg] = o.off;
        c->ip = (uint16_t)next;
        break;
    case K_JCC:
        target = pmf_cond(op & 15, c->flags) ? ((next + imm) & 0xffffu) : next;
        if (target != next && !pmf_ok(P, PC98_CS, target, 1, PC98_SEG_EXECUTE, &pb)) return 0;
        c->ip = (uint16_t)target;
        break;
    case K_LOOP: {
        const uint16_t cx = op == 0xe3 ? c->r[PC98_CX] : (uint16_t)(c->r[PC98_CX] - 1);
        unsigned take;
        if (op == 0xe3) take = cx == 0;
        else {
            take = cx != 0;
            if (op == 0xe0) take = take && !(c->flags & PC98_ZF);
            if (op == 0xe1) take = take && (c->flags & PC98_ZF);
        }
        target = take ? ((next + imm) & 0xffffu) : next;
        if (take && !pmf_ok(P, PC98_CS, target, 1, PC98_SEG_EXECUTE, &pb)) {
            /* the reference decrements CX and then faults on the target, rolling both back: leave it all to it */
            return 0;
        }
        c->r[PC98_CX] = cx;
        c->ip = (uint16_t)target;
        break;
    }
    case K_JMP:
        target = (next + imm) & 0xffffu;
        if (!pmf_ok(P, PC98_CS, target, 1, PC98_SEG_EXECUTE, &pb)) return 0;
        c->ip = (uint16_t)target;
        break;
    case K_CALL: {
        const uint16_t sp = (uint16_t)(c->r[PC98_SP] - 2);
        target = (next + imm) & 0xffffu;
        if (!pmf_ok(P, PC98_SS, sp, 2, PC98_SEG_WRITE, &pa) || !pmf_ok(P, PC98_CS, target, 1, PC98_SEG_EXECUTE, &pb)) return 0;
        pmf_wr(pa, 16, next);
        c->r[PC98_SP] = sp;
        c->ip = (uint16_t)target;
        break;
    }
    case K_RET:
        if (!pmf_ok(P, PC98_SS, c->r[PC98_SP], 2, PC98_SEG_READ, &pa)) return 0;
        target = pmf_rd(pa, 16);
        if (!pmf_ok(P, PC98_CS, target, 1, PC98_SEG_EXECUTE, &pb)) return 0;
        c->r[PC98_SP] = (uint16_t)(c->r[PC98_SP] + 2);
        c->ip = (uint16_t)target;
        break;
    case K_CBW:
        c->r[PC98_AX] = (uint16_t)(int16_t)(int8_t)(c->r[PC98_AX] & 255u);
        c->ip = (uint16_t)next;
        break;
    case K_CWD:
        c->r[PC98_DX] = (c->r[PC98_AX] & 0x8000u) ? 0xffffu : 0;
        c->ip = (uint16_t)next;
        break;
    case K_FLAG:
        if (op == 0xf5) c->flags ^= PC98_CF;
        else if (op == 0xf8) c->flags &= (uint16_t)~PC98_CF;
        else if (op == 0xf9) c->flags |= PC98_CF;
        else if (op == 0xfc) c->flags &= (uint16_t)~PC98_DF;
        else c->flags |= PC98_DF;
        c->ip = (uint16_t)next;
        break;
    case K_SHIFT: {
        const unsigned l = o.is_reg ? pmf_reg(c, o.rm, bits) : pmf_rd(pa, bits);
        const unsigned r = pc98_cpu_shift(o.reg, bits, (uint16_t)l, cnt, &c->flags);
        if (o.is_reg) pmf_setreg(c, o.rm, bits, r); else pmf_wr(pa, bits, r);
        c->ip = (uint16_t)next;
        break;
    }
    case K_STR: {
        const unsigned src = op <= 0xa5 || op >= 0xac, dst = op <= 0xab;
        const unsigned sseg = seg < 0 ? PC98_DS : (unsigned)seg;
        const uint16_t delta = (uint16_t)((c->flags & PC98_DF) ? 0u - bits / 8 : bits / 8);
        unsigned v;

        if (rep && !c->r[PC98_CX]) { c->ip = (uint16_t)next; break; }
        if (src && !pmf_ok(P, sseg, c->r[PC98_SI], bits / 8, PC98_SEG_READ, &pa)) return 0;
        if (dst && !pmf_ok(P, PC98_ES, c->r[PC98_DI], bits / 8, PC98_SEG_WRITE, &pb)) return 0;
        v = src ? pmf_rd(pa, bits) : pmf_reg(c, PC98_AX, bits);
        if (dst) pmf_wr(pb, bits, v); else pmf_setreg(c, PC98_AX, bits, v);
        if (src) c->r[PC98_SI] = (uint16_t)(c->r[PC98_SI] + delta);
        if (dst) c->r[PC98_DI] = (uint16_t)(c->r[PC98_DI] + delta);
        c->ip = (uint16_t)next;
        if (rep) {
            c->r[PC98_CX] = (uint16_t)(c->r[PC98_CX] - 1);
            if (c->r[PC98_CX]) { c->ip = (uint16_t)ip0; completed = 0; }
        }
        break;
    }
    }

    if (completed) {
        if (c->irq_inhibit) c->irq_inhibit--;
        if (c->nmi_inhibit) c->nmi_inhibit--;
        if (c->debug_inhibit) c->debug_inhibit--;
    }
    /* the clock classifier reads the instruction bytes the way the reference fetch records them */
    A->nfetch = pos;
    memcpy(A->ib, ib, sizeof(A->ib));
    *clk = clocks_of(A, ip0, cx0, c->ip, cl0);
    A->retired++;
    return 1;
}
