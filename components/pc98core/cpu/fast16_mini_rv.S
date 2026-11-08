/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * fast16 mini loop for the ESP32-P4 (RISC-V), stage 21. Same contract as fast16_mini.inc (which stays the reference,
 * and runs the opcodes not written here one at a time through f16x_cone), but laid out for this core instead of
 * following the C:
 *  - one function, f16x_start; every handler is a label in it, entered with jr, and the loop state stays in the
 *    callee-saved registers (a C call in between keeps them):
 *        s0 A   s1 pc (host pointer to the opcode)   s2 rem   s3 LR   s4 lim   s5 T   s6 ram   s7 nret   s8 LV
 *        s9 R: the handler's record (T entry of its opcode)   s10 code base (A->mo_pcb)   s11 FLAGS system bits
 *    constants in a1 a3-a7 (put back after a C call: KLOAD), a2 the segment base named by a prefix,
 *    t0-t6 a0 ra scratch (t5 ModR/M, t6 the clock of the instruction, ra the EA stub link)
 *  - lazy flags in two registers instead of FLAGS:
 *        LR = the last result shifted to the top (word << 16, byte << 24): ZF = (LR == 0), SF = (LR < 0)
 *        LV = bit 31 CF, bit 30 CF ^ OF, AF at bit 19 (word result) or 27 (byte), bit 1: AF at 27,
 *             bit 0: LR holds a byte (PF from bits 24-31, else 16-23); other bits are don't-care
 *    so JC / JZ / JS are one branch, and ADD / SUB set the flags in four instructions (five for a byte). FLAGS is
 *    built only for PUSHF / LAHF, the C calls and the exit. A FLAGS value with ZF set and SF set or PF clear has no
 *    LR (only POPF / SAHF / IRET can make one): the loop is not entered with it, and SAHF hands it back.
 *  - T = f16x_rec (fast16.c): 16 bytes per index i: +0 handler of opcode i, +4 / +5 byte register offsets of the
 *    r/m / reg field of ModR/M i, +6 PF of byte i (4 / 0), +7 / +8 clock of opcode i (register form or no ModR/M /
 *    memory form), +12 handler of opcode i after a segment prefix. Dispatch is lbu / slli / add / lw / jr and leaves
 *    the record in s9, so a handler loads its clock with one lbu.
 *  - a hand-back (f16x_exit) happens before any side effect, with pc at the opcode (or at the prefix: f16x_bailov)
 *  - memory: as fast16_mini.inc (RAM below A0000h for writes, A4000h for reads, no 64K wrap); a byte move to plain
 *    graphics VRAM calls the bus handler (gvram_plain_rd8 / wr8) with the clock handed over, as the C one.
 *  - PC98_FAST16_WRITE_HOOK (the differential test): every RAM store is reported before it happens, every retired
 *    instruction to t_trace. */
#include "fast16_rv.h"

#define rA    s0
#define rPC   s1
#define rREM  s2
#define rLR   s3
#define rLIM  s4
#define rT    s5
#define rRAM  s6
#define rN    s7
#define rLV   s8
#define rR    s9
#define rPCB  s10
#define rFS   s11
#define kC0   a1      /* C0h: ModR/M register form from here */
#define rOVB  a2      /* segment base named by the prefix (handlers entered after one) */
#define kREND a3      /* A4000h: reads below */
#define kWEND a4      /* A0000h: writes below */
#define kAF   a5      /* LV bits kept by an instruction that leaves AF: both AF places, the AF place bit */
#define kEAT  a6      /* f16x_eat */
#define kFFFF a7

#define REC_RMB  4
#define REC_REGB 5
#define REC_PF   6
#define REC_CR   7
#define REC_CM   8
#define REC_HOV  12

/* stack frame: locals at the bottom, the saved registers above */
#define FRAME     80
#define FS_T1     0
#define FS_T2     4
#define FS_T3     8
#define FS_MCKM8  12      /* A->mck_main + 800h */

    .set xov, 0

/* ---------------- test hooks ---------------- */
#ifdef PC98_FAST16_WRITE_HOOK
.macro SAVET
    addi sp, sp, -64
    sw ra, 0(sp)
    sw t0, 4(sp)
    sw t1, 8(sp)
    sw t2, 12(sp)
    sw t3, 16(sp)
    sw t4, 20(sp)
    sw t5, 24(sp)
    sw t6, 28(sp)
    sw a0, 32(sp)
    sw a1, 36(sp)
    sw a2, 40(sp)
    sw a3, 44(sp)
    sw a4, 48(sp)
    sw a5, 52(sp)
    sw a6, 56(sp)
    sw a7, 60(sp)
.endm
.macro RESTT
    lw ra, 0(sp)
    lw t0, 4(sp)
    lw t1, 8(sp)
    lw t2, 12(sp)
    lw t3, 16(sp)
    lw t4, 20(sp)
    lw t5, 24(sp)
    lw t6, 28(sp)
    lw a0, 32(sp)
    lw a1, 36(sp)
    lw a2, 40(sp)
    lw a3, 44(sp)
    lw a4, 48(sp)
    lw a5, 52(sp)
    lw a6, 56(sp)
    lw a7, 60(sp)
    addi sp, sp, 64
.endm
/* a store of byte hi of val at ptr (+hi); not reported when ptr is not in RAM (a register inside A) */
.macro HOOK1 ptr, val, hi
    SAVET
    sub ra, \ptr, rRAM
  .if \hi
    srli a1, \val, 8
    addi ra, ra, 1
  .else
    mv a1, \val
  .endif
    andi a1, a1, 255
    mv a0, ra
    li t0, 0x200000
    bgeu a0, t0, .Lh\@
    call PC98_FAST16_WRITE_HOOK
.Lh\@:
    RESTT
.endm
.macro TRACE
    SAVET
    mv a0, rPC
    call t_trace
    RESTT
.endm
#else
.macro HOOK1 ptr, val, hi
.endm
.macro TRACE
.endm
#endif

/* ---------------- dispatch / retire ---------------- */
.macro DISPATCH
    lbu t0, 0(rPC)
    slli t0, t0, 4
    add rR, t0, rT
    lw t0, 0(rR)
    jr t0
.endm
/* exit islands: every retire is followed by one, so a hand-back branch (98f / 98b, 97 after a prefix) stays short */
.macro ISLAND
98: j f16x_exit
  .if xov
97: j f16x_bailov
  .endif
.endm
/* retire: debit t6 clocks, advance pc by nb (immediate, 0: already set), continue unless the batch is over.
 * The next opcode is read after the last store of the instruction (no stale prefetch on self-modifying code). */
.macro RETIRE nb
    lbu t0, \nb(rPC)
    sub rREM, rREM, t6
    slli t0, t0, 4
    addi rN, rN, 1
    add rR, t0, rT
  .if \nb
    addi rPC, rPC, \nb
  .endif
    lw t0, 0(rR)
    TRACE
    blez rREM, 98f
    bgeu rPC, rLIM, 98f
    jr t0
    ISLAND
.endm
.macro RETIRER nbr
    add rPC, rPC, \nbr
    RETIRE 0
.endm
/* the constants (after a C call) */
.macro KLOAD
    li kFFFF, 0xffff
    la kEAT, f16x_eat
    li kAF, 0x08080002
    li kWEND, 0xa0000
    li kREND, 0xa4000
    li kC0, 0xc0
.endm
/* hand-back branch to the nearest island in direction d (b / f): 98 (plain), 97 (after a prefix) */
.macro BX insn, r1, r2, d
  .if xov
    \insn \r1, \r2, 97\d
  .else
    \insn \r1, \r2, 98\d
  .endif
.endm
.macro BX1 insn, r1, d
  .if xov
    \insn \r1, 97\d
  .else
    \insn \r1, 98\d
  .endif
.endm

/* ---------------- lazy flags ---------------- */
/* t = PF (4 / 0) of the last result's low byte */
.macro PFB t
    andi \t, rLV, 1
    slli \t, \t, 3
    addi \t, \t, 16
    srl \t, rLR, \t
    andi \t, \t, 255
    slli \t, \t, 4
    add \t, \t, rT
    lbu \t, REC_PF(\t)
.endm
/* d = FLAGS (t scratch) */
.macro MATFL d, t
    srli \d, rLV, 31
    or \d, \d, rFS
    slli \t, rLV, 1
    xor \t, \t, rLV
    srli \t, \t, 31
    slli \t, \t, 11
    or \d, \d, \t
    andi \t, rLV, 2
    slli \t, \t, 2
    addi \t, \t, 19
    srl \t, rLV, \t
    andi \t, \t, 1
    slli \t, \t, 4
    or \d, \d, \t
    srli \t, rLR, 31
    slli \t, \t, 7
    or \d, \d, \t
    seqz \t, rLR
    slli \t, \t, 6
    or \d, \d, \t
    PFB \t
    or \d, \d, \t
.endm
/* LR from ZF SF PF of f (a byte result: 0, 1, 3, FDh, FFh); to fail (nothing changed) when there is none */
.macro LAZYLR f, fail, t
    andi \t, \f, 0x40
    bnez \t, .Lz\@
    srli \t, \f, 1
    andi \t, \t, 2
    ori rLR, \t, 1
    andi \t, \f, 0x80
    beqz \t, .Ls\@
    ori rLR, rLR, 0xfc
.Ls\@:
    slli rLR, rLR, 24
    j .Le\@
.Lz\@:
    andi \t, \f, 0x84
    addi \t, \t, -4
    bnez \t, \fail
    li rLR, 0
.Le\@:
.endm
/* LR LV and the system bits from FLAGS f (t scratch); to fail (nothing changed) when there is no LR */
.macro LAZYFL f, t, fail
    LAZYLR \f, \fail, \t
    slli rLV, \f, 31
    srli \t, \f, 11
    xor \t, \t, \f
    slli \t, \t, 31
    srli \t, \t, 1
    or rLV, rLV, \t
    srli \t, \f, 4
    andi \t, \t, 1
    slli \t, \t, 27
    or rLV, rLV, \t
    ori rLV, rLV, 3
    li \t, ~0x8d5
    and rFS, \f, \t
.endm
/* add / sub flags: r = x +- y (+- carry) unmasked, v scratch (x y r kept) */
.macro FLA w, x, y, r, v
    xor \v, \x, \y
    xor \v, \v, \r
  .if \w == 16
    slli rLV, \v, 15
    slli rLR, \r, 16
  .else
    slli rLV, \v, 23
    slli rLR, \r, 24
    ori rLV, rLV, 3
  .endif
.endm
/* OR AND XOR TEST: CF = OF = 0, AF kept */
.macro FLOG w, r
    and rLV, rLV, kAF
  .if \w == 16
    slli rLR, \r, 16
  .else
    ori rLV, rLV, 1
    slli rLR, \r, 24
  .endif
.endm
/* ALU operation aop (0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP): r = x op y with the flags; r v distinct from x y */
.macro ALUOP aop, w, r, x, y, v
  .if \aop == 0
    add \r, \x, \y
  .elseif \aop == 1
    or \r, \x, \y
  .elseif \aop == 2
    srli \v, rLV, 31
    add \r, \x, \y
    add \r, \r, \v
  .elseif \aop == 3
    srli \v, rLV, 31
    sub \r, \x, \y
    sub \r, \r, \v
  .elseif \aop == 4
    and \r, \x, \y
  .elseif \aop == 6
    xor \r, \x, \y
  .else
    sub \r, \x, \y
  .endif
  .if (\aop == 1) || (\aop == 4) || (\aop == 6)
    FLOG \w, \r
  .else
    FLA \w, \x, \y, \r, \v
  .endif
.endm
/* INC / DEC flags: r = x +- 1 unmasked; CF kept (and OF with it: bit 30 follows); v u scratch */
.macro FLID w, x, r, v, u
    xor \v, \x, \r
  .if \w == 16
    slli \v, \v, 15
    slli rLR, \r, 16
  .else
    slli \v, \v, 23
    slli rLR, \r, 24
    ori \v, \v, 3
  .endif
    xor \u, \v, rLV
    srai \u, \u, 31
    slli \u, \u, 30
    xor rLV, \v, \u
.endm
/* shifts / rotates by 1: CF = cf, OF = cf ^ x30 (0 / 1, clobbered), AF kept; lr 0: LR kept (rotate),
 * 8 / 16: the LR format of a new byte / word result (set by the caller); t scratch */
.macro ROTFL cf, x30, t, lr
    slli \cf, \cf, 31
    slli \x30, \x30, 30
    or \cf, \cf, \x30
  .if \lr == 0
    andi \t, rLV, 1
    and rLV, rLV, kAF
    or rLV, rLV, \t
  .elseif \lr == 8
    and rLV, rLV, kAF
    ori rLV, rLV, 1
  .else
    and rLV, rLV, kAF
  .endif
    or rLV, rLV, \cf
.endm

/* ---------------- memory ---------------- */
.macro LDW d, p, t
    lbu \d, 0(\p)
    lbu \t, 1(\p)
    slli \t, \t, 8
    or \d, \d, \t
.endm
.macro STB p, v
    HOOK1 \p, \v, 0
    sb \v, 0(\p)
.endm
.macro STW p, v, t
    HOOK1 \p, \v, 0
    HOOK1 \p, \v, 1
    sb \v, 0(\p)
    srli \t, \v, 8
    sb \t, 1(\p)
.endm
/* ModR/M memory operand, t5 = ModR/M (< C0h): t0 offset, t1 linear address, t2 opcode+ModR/M+disp length */
.macro EA
    slli t0, t5, 2
    add t0, t0, kEAT
    lw t0, 0(t0)
    jalr ra, t0
  .if xov
    add t1, rOVB, t0
  .else
    add t1, t1, t0
  .endif
.endm
/* operand bounds (t0 offset, t1 linear; t3 scratch): word / byte, ro = read only; hand-back to the island in d */
.macro CHKW ro, d
    BX beq, t0, kFFFF, \d              /* offset FFFFh: 64K wrap */
    addi t3, t1, 1
  .if \ro
    BX bgeu, t3, kREND, \d
  .else
    BX bgeu, t3, kWEND, \d
  .endif
.endm
.macro CHKB ro, d
  .if \ro
    BX bgeu, t1, kREND, \d
  .else
    BX bgeu, t1, kWEND, \d
  .endif
.endm
/* register pointers from ModR/M m: word rm / reg (A + 2 * index), byte rm / reg (A + byte offset) */
.macro RMW d, m
    andi \d, \m, 7
    slli \d, \d, 1
    add \d, \d, rA
.endm
.macro REGW d, m
    srli \d, \m, 2
    andi \d, \d, 14
    add \d, \d, rA
.endm
.macro REGB d, m
    slli \d, \m, 4
    add \d, \d, rT
    lbu \d, REC_REGB(\d)
    add \d, \d, rA
.endm
.macro RMB d, m
    slli \d, \m, 4
    add \d, \d, rT
    lbu \d, REC_RMB(\d)
    add \d, \d, rA
.endm
/* the same as pointers to the register itself (offset of the register file added): byte (B 8) or word (B 16) */
.macro RMP d, m, B
  .if \B == 8
    RMB \d, \m
  .else
    RMW \d, \m
  .endif
    addi \d, \d, F16A_R
.endm
/* clock from mck_main[(op << 4) | (reg << 1) | mem] (op 80h-FFh): d = clock, reg in r, x scratch */
.macro MAINCLK d, op, r, mem, x
    lw \d, FS_MCKM8(sp)
    slli \x, \r, 1
    add \d, \d, \x
    lbu \d, (((\op) << 4) - 0x800 + (\mem))(\d)
.endm
/* new ip = (ip + nb + d) & FFFFh: pc = pcb + that; d register, t scratch */
.macro JREL nb, d, t
    sub \t, rPC, rPCB
    addi \t, \t, \nb
    add \t, \t, \d
    and \t, \t, kFFFF
    add rPC, rPCB, \t
.endm
/* segment register sg (constant 0..3) = v (16 bits): seg, base, and the descriptor cache in the 286 models; t u scratch */
.macro LOADSEGK sg, v, t, u, acc=0x93
    sh \v, F16A_SEG+2*(\sg)(rA)
    slli \u, \v, 4
    sw \u, F16A_BASE+4*(\sg)(rA)
    lw \t, F16A_MENU(rA)
    beqz \t, .Lls\@
    sw \u, F16A_CACHE+8*(\sg)(rA)
    sh kFFFF, F16A_CACHE+8*(\sg)+4(rA)
    li \u, \acc
    sb \u, F16A_CACHE+8*(\sg)+6(rA)
    li \u, 1
    sb \u, F16A_VALID+(\sg)(rA)
.Lls\@:
.endm
/* the same with sg in a register (kept); t u scratch */
.macro LOADSEG sg, v, t, u
    slli \t, \sg, 1
    add \t, \t, rA
    sh \v, F16A_SEG(\t)
    slli \u, \v, 4
    slli \t, \sg, 2
    add \t, \t, rA
    sw \u, F16A_BASE(\t)
    lw \t, F16A_MENU(rA)
    beqz \t, .Lls\@
    slli \t, \sg, 3
    add \t, \t, rA
    sw \u, F16A_CACHE(\t)
    sh kFFFF, F16A_CACHE+4(\t)
    li \u, 0x93
    sb \u, F16A_CACHE+6(\t)
    add \t, \sg, rA
    li \u, 1
    sb \u, F16A_VALID(\t)
.Lls\@:
.endm
/* jump on r (0..7, clobbered) to the numeric labels 20..27 through a table; t scratch */
.macro JT8 r, t, tab
    .pushsection .dram1.f16x_jt, "aw"
    .balign 4
\tab:
    .word 20f, 21f, 22f, 23f, 24f, 25f, 26f, 27f
    .popsection
    la \t, \tab
    slli \r, \r, 2
    add \t, \t, \r
    lw \t, 0(\t)
    jr \t
.endm

    .section .iram1.f16x, "ax"
    .balign 4
    .globl f16x_start
    .type f16x_start, @function
/* void f16x_start(Pc98Adapter *A, const uint8_t *pc, int32_t rem, unsigned fl, const uint8_t *lim, uint8_t *ram) */
f16x_start:
    addi sp, sp, -FRAME
    sw ra, 76(sp)
    sw s0, 72(sp)
    sw s1, 68(sp)
    sw s2, 64(sp)
    sw s3, 60(sp)
    sw s4, 56(sp)
    sw s5, 52(sp)
    sw s6, 48(sp)
    sw s7, 44(sp)
    sw s8, 40(sp)
    sw s9, 36(sp)
    sw s10, 32(sp)
    sw s11, 28(sp)
    mv rA, a0
    mv rPC, a1
    mv rREM, a2
    mv rLIM, a4
    mv rRAM, a5
    la rT, f16x_rec
    lw rPCB, F16A_MOPCB(rA)
    li rN, 0
    lw t0, F16A_MCKMAIN(rA)
    addi t0, t0, 0x400
    addi t0, t0, 0x400
    sw t0, FS_MCKM8(sp)
    LAZYFL a3, t0, .Lrefuse
    KLOAD
    DISPATCH
.Lrefuse:                              /* FLAGS has no LR: nothing done, the generic loop goes on */
    sub t1, rPC, rPCB
    sw t1, F16A_MOIP(rA)
    sw rREM, F16A_MOREM(rA)
    sw a3, F16A_MOFL(rA)
    sw rN, F16A_MONRET(rA)
    j f16x_ret

/* hand back (as C m_exit): pc, clock, FLAGS, count */
    .balign 4
    .globl f16x_h_exit
f16x_h_exit:
f16x_exit:
    MATFL t0, t1
    sub t1, rPC, rPCB
    sw t1, F16A_MOIP(rA)
    sw rREM, F16A_MOREM(rA)
    sw t0, F16A_MOFL(rA)
    sw rN, F16A_MONRET(rA)
f16x_ret:                              /* the A->mo_* state is stored */
    lw ra, 76(sp)
    lw s0, 72(sp)
    lw s1, 68(sp)
    lw s2, 64(sp)
    lw s3, 60(sp)
    lw s4, 56(sp)
    lw s5, 52(sp)
    lw s6, 48(sp)
    lw s7, 44(sp)
    lw s8, 40(sp)
    lw s9, 36(sp)
    lw s10, 32(sp)
    lw s11, 28(sp)
    addi sp, sp, FRAME
    ret
/* hand back from a handler entered after a segment prefix: pc goes back to the prefix */
    .balign 4
    .globl f16x_h_bailov
f16x_h_bailov:
f16x_bailov:
    addi rPC, rPC, -1
    j f16x_exit

/* one instruction through the C mini handlers (f16x_cone), then on unless it ended the batch or changed CS */
    .balign 4
    .globl f16x_h_cstep
f16x_h_cstep:
    MATFL a3, t0
    mv a0, rA
    mv a1, rPC
    mv a2, rREM
    mv a4, rN
    call f16x_cone
    lw t0, F16A_MONRET(rA)
    beq t0, rN, f16x_ret               /* handed back: the generic loop runs it */
    lw t1, F16A_MOEV(rA)
    bnez t1, f16x_ret
    lw t1, F16A_MOPCB(rA)
    bne t1, rPCB, f16x_ret
    lw t2, F16A_MOREM(rA)
    blez t2, f16x_ret
    lw t3, F16A_MOIP(rA)
    add t3, rPCB, t3
    bgeu t3, rLIM, f16x_ret
    lw t4, F16A_MOFL(rA)
    LAZYFL t4, t1, f16x_ret
    mv rN, t0
    mv rREM, t2
    mv rPC, t3
    KLOAD
    DISPATCH

/* ---------------- EA stubs: jalr ra; out t0 offset, t1 default segment base, t2 length ---------------- */
.macro EASTUB mod, rm
    .balign 4
x_ea_\mod\rm:
  .if (\mod == 0) && (\rm == 6)
    lbu t0, 2(rPC)
    lbu t1, 3(rPC)
    li t2, 4
    slli t1, t1, 8
    or t0, t0, t1
    lw t1, F16A_BASE+12(rA)
    ret
  .else
    .if (\rm == 0) || (\rm == 1) || (\rm == 7)
    lhu t0, F16A_R+6(rA)
    .elseif (\rm == 2) || (\rm == 3) || (\rm == 6)
    lhu t0, F16A_R+10(rA)
    .elseif \rm == 4
    lhu t0, F16A_R+12(rA)
    .else
    lhu t0, F16A_R+14(rA)
    .endif
    .if \rm < 4
    lhu t1, F16A_R+12+2*(\rm & 1)(rA)
    add t0, t0, t1
    .endif
    .if \mod == 1
    lb t1, 2(rPC)
    add t0, t0, t1
    .elseif \mod == 2
    lbu t1, 2(rPC)
    lbu t2, 3(rPC)
    add t0, t0, t1
    slli t2, t2, 8
    add t0, t0, t2
    .endif
    .if (\mod != 0) || (\rm < 4)
    and t0, t0, kFFFF
    .endif
    .if (\rm == 2) || (\rm == 3) || (\rm == 6)
    lw t1, F16A_BASE+8(rA)
    .else
    lw t1, F16A_BASE+12(rA)
    .endif
    li t2, 2+\mod
    ret
  .endif
.endm
  .irp mod, 0, 1, 2
  .irp rm, 0, 1, 2, 3, 4, 5, 6, 7
    EASTUB \mod, \rm
  .endr
  .endr

/* ---------------- 00-3B: ALU r/m, r and r, r/m (form bit 0 word, bit 1 reg is the destination) ---------------- */
.macro ALU_RM lbl, aop, form, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
  .if \form & 1
    RMW t0, t5
    REGW t1, t5
    lhu t2, F16A_R(t0)
    lhu t3, F16A_R(t1)
  .else
    slli t0, t5, 4
    add t0, t0, rT
    lbu t1, REC_REGB(t0)
    lbu t0, REC_RMB(t0)
    add t1, t1, rA
    add t0, t0, rA
    lbu t2, F16A_R(t0)
    lbu t3, F16A_R(t1)
  .endif
    lbu t6, REC_CR(rR)
  .if \form & 2
    ALUOP \aop, 8+8*(\form & 1), t4, t3, t2, a0
    .if \aop != 7
      .if \form & 1
    sh t4, F16A_R(t1)
      .else
    sb t4, F16A_R(t1)
      .endif
    .endif
  .else
    ALUOP \aop, 8+8*(\form & 1), t4, t2, t3, a0
    .if \aop != 7
      .if \form & 1
    sh t4, F16A_R(t0)
      .else
    sb t4, F16A_R(t0)
      .endif
    .endif
  .endif
    RETIRE 2
.Lm\@:
    EA
  .if \form & 1
    CHKW (\form & 2) || (\aop == 7), b
  .else
    CHKB (\form & 2) || (\aop == 7), b
  .endif
    add rPC, rPC, t2
    add t1, t1, rRAM
  .if \form & 1
    LDW t2, t1, t3
    REGW t3, t5
    lhu t4, F16A_R(t3)
  .else
    lbu t2, 0(t1)
    REGB t3, t5
    lbu t4, F16A_R(t3)
  .endif
    lbu t6, REC_CM(rR)
  .if \form & 2
    ALUOP \aop, 8+8*(\form & 1), a0, t4, t2, t0
    .if \aop != 7
      .if \form & 1
    sh a0, F16A_R(t3)
      .else
    sb a0, F16A_R(t3)
      .endif
    .endif
  .else
    ALUOP \aop, 8+8*(\form & 1), a0, t2, t4, t0
    .if \aop != 7
      .if \form & 1
    STW t1, a0, t0
      .else
    STB t1, a0
      .endif
    .endif
  .endif
    RETIRE 0
    .set xov, 0
.endm
    ALU_RM x_00, 0, 0, 0
    ALU_RM x_01, 0, 1, 0
    ALU_RM x_02, 0, 2, 0
    ALU_RM x_03, 0, 3, 0
    ALU_RM xo_00, 0, 0, 1
    ALU_RM xo_01, 0, 1, 1
    ALU_RM xo_02, 0, 2, 1
    ALU_RM xo_03, 0, 3, 1
    ALU_RM x_08, 1, 0, 0
    ALU_RM x_09, 1, 1, 0
    ALU_RM x_0a, 1, 2, 0
    ALU_RM x_0b, 1, 3, 0
    ALU_RM xo_08, 1, 0, 1
    ALU_RM xo_09, 1, 1, 1
    ALU_RM xo_0a, 1, 2, 1
    ALU_RM xo_0b, 1, 3, 1
    ALU_RM x_10, 2, 0, 0
    ALU_RM x_11, 2, 1, 0
    ALU_RM x_12, 2, 2, 0
    ALU_RM x_13, 2, 3, 0
    ALU_RM xo_10, 2, 0, 1
    ALU_RM xo_11, 2, 1, 1
    ALU_RM xo_12, 2, 2, 1
    ALU_RM xo_13, 2, 3, 1
    ALU_RM x_18, 3, 0, 0
    ALU_RM x_19, 3, 1, 0
    ALU_RM x_1a, 3, 2, 0
    ALU_RM x_1b, 3, 3, 0
    ALU_RM xo_18, 3, 0, 1
    ALU_RM xo_19, 3, 1, 1
    ALU_RM xo_1a, 3, 2, 1
    ALU_RM xo_1b, 3, 3, 1
    ALU_RM x_20, 4, 0, 0
    ALU_RM x_21, 4, 1, 0
    ALU_RM x_22, 4, 2, 0
    ALU_RM x_23, 4, 3, 0
    ALU_RM xo_20, 4, 0, 1
    ALU_RM xo_21, 4, 1, 1
    ALU_RM xo_22, 4, 2, 1
    ALU_RM xo_23, 4, 3, 1
    ALU_RM x_28, 5, 0, 0
    ALU_RM x_29, 5, 1, 0
    ALU_RM x_2a, 5, 2, 0
    ALU_RM x_2b, 5, 3, 0
    ALU_RM xo_28, 5, 0, 1
    ALU_RM xo_29, 5, 1, 1
    ALU_RM xo_2a, 5, 2, 1
    ALU_RM xo_2b, 5, 3, 1
    ALU_RM x_30, 6, 0, 0
    ALU_RM x_31, 6, 1, 0
    ALU_RM x_32, 6, 2, 0
    ALU_RM x_33, 6, 3, 0
    ALU_RM xo_30, 6, 0, 1
    ALU_RM xo_31, 6, 1, 1
    ALU_RM xo_32, 6, 2, 1
    ALU_RM xo_33, 6, 3, 1
    ALU_RM x_38, 7, 0, 0
    ALU_RM x_39, 7, 1, 0
    ALU_RM x_3a, 7, 2, 0
    ALU_RM x_3b, 7, 3, 0
    ALU_RM xo_38, 7, 0, 1
    ALU_RM xo_39, 7, 1, 1
    ALU_RM xo_3a, 7, 2, 1
    ALU_RM xo_3b, 7, 3, 1

/* ---------------- ALU AL / AX, imm ---------------- */
.macro ALU_ACC lbl, aop, w
    .balign 4
\lbl:
    lbu t6, REC_CR(rR)
  .if \w == 16
    lhu t1, F16A_R(rA)
    lbu t2, 1(rPC)
    lbu t3, 2(rPC)
    slli t3, t3, 8
    or t2, t2, t3
    ALUOP \aop, 16, t3, t1, t2, t4
    .if \aop != 7
    sh t3, F16A_R(rA)
    .endif
    RETIRE 3
  .else
    lbu t1, F16A_R(rA)
    lbu t2, 1(rPC)
    ALUOP \aop, 8, t3, t1, t2, t4
    .if \aop != 7
    sb t3, F16A_R(rA)
    .endif
    RETIRE 2
  .endif
.endm
    ALU_ACC x_04, 0, 8
    ALU_ACC x_05, 0, 16
    ALU_ACC x_0c, 1, 8
    ALU_ACC x_0d, 1, 16
    ALU_ACC x_14, 2, 8
    ALU_ACC x_15, 2, 16
    ALU_ACC x_1c, 3, 8
    ALU_ACC x_1d, 3, 16
    ALU_ACC x_24, 4, 8
    ALU_ACC x_25, 4, 16
    ALU_ACC x_2c, 5, 8
    ALU_ACC x_2d, 5, 16
    ALU_ACC x_34, 6, 8
    ALU_ACC x_35, 6, 16
    ALU_ACC x_3c, 7, 8
    ALU_ACC x_3d, 7, 16

/* ---------------- INC / DEC r16 (CF kept) ---------------- */
.macro INCDEC lbl, i, dec
    .balign 4
\lbl:
    lhu t1, F16A_R+2*\i(rA)
    lbu t6, REC_CR(rR)
  .if \dec
    addi t2, t1, -1
  .else
    addi t2, t1, 1
  .endif
    sh t2, F16A_R+2*\i(rA)
    FLID 16, t1, t2, t3, t4
    RETIRE 1
.endm
  .irp i, 0, 1, 2, 3, 4, 5, 6, 7
    INCDEC x_4\i, \i, 0
  .endr
    INCDEC x_48, 0, 1
    INCDEC x_49, 1, 1
    INCDEC x_4a, 2, 1
    INCDEC x_4b, 3, 1
    INCDEC x_4c, 4, 1
    INCDEC x_4d, 5, 1
    INCDEC x_4e, 6, 1
    INCDEC x_4f, 7, 1

/* ---------------- PUSH / POP ---------------- */
/* push: t1 host address of the new top, t2 new SP; hand-back when SP = 1 or the word is not below A0000h */
.macro PUSHCHK d=f
    lhu t0, F16A_R+8(rA)
    lw t1, F16A_BASE+8(rA)
    addi t2, t0, -2
    addi t3, t0, -1
    and t2, t2, kFFFF
    beqz t3, 98\d
    add t1, t1, t2
    addi t3, t1, 1
    bgeu t3, kWEND, 98\d
    add t1, t1, rRAM
.endm
/* pop: t1 host address of the top, t0 SP; hand-back when SP = FFFFh or the word is not below A4000h */
.macro POPCHK
    lhu t0, F16A_R+8(rA)
    lw t1, F16A_BASE+8(rA)
    add t1, t1, t0
    addi t3, t1, 1
    bgeu t3, kREND, 98f
    beq t0, kFFFF, 98f
    add t1, t1, rRAM
.endm
.macro PUSHR lbl, off
    .balign 4
\lbl:
    PUSHCHK
    lhu t3, \off(rA)
    lbu t6, REC_CR(rR)
    STW t1, t3, t4
    sh t2, F16A_R+8(rA)
    RETIRE 1
.endm
.macro POPR lbl, i
    .balign 4
\lbl:
    POPCHK
    LDW t3, t1, t4
    addi t0, t0, 2
    lbu t6, REC_CR(rR)
    sh t0, F16A_R+8(rA)
    sh t3, F16A_R+2*\i(rA)
    RETIRE 1
.endm
    PUSHR x_50, F16A_R+0
    PUSHR x_51, F16A_R+2
    PUSHR x_52, F16A_R+4
    PUSHR x_53, F16A_R+6
    PUSHR x_55, F16A_R+10
    PUSHR x_56, F16A_R+12
    PUSHR x_57, F16A_R+14
    POPR x_58, 0
    POPR x_59, 1
    POPR x_5a, 2
    POPR x_5b, 3
    POPR x_5d, 5
    POPR x_5e, 6
    POPR x_5f, 7
    PUSHR x_06, F16A_SEG+0
    PUSHR x_0e, F16A_SEG+2
    PUSHR x_16, F16A_SEG+4
    PUSHR x_1e, F16A_SEG+6
.macro POPSEG lbl, sg
    .balign 4
\lbl:
    POPCHK
    LDW t3, t1, t4
    addi t0, t0, 2
    sh t0, F16A_R+8(rA)
    LOADSEGK \sg, t3, t0, t1
    lbu t6, REC_CR(rR)
    RETIRE 1
.endm
    POPSEG x_07, 0
    POPSEG x_1f, 3

/* PUSHF: the V30 (menu 0) sets the top four bits (hand-back to the island before it) */
    ISLAND
    .balign 4
x_9c:
    PUSHCHK b
    MATFL t3, t4
    lw t4, F16A_MENU(rA)
    bnez t4, 1f
    li t4, 0xfd5
    and t3, t3, t4
    li t4, 0xf002
    or t3, t3, t4
    j 2f
1:  li t4, 0x7fd5
    and t3, t3, t4
    ori t3, t3, 2
2:  STW t1, t3, t4
    sh t2, F16A_R+8(rA)
    lbu t6, REC_CR(rR)
    RETIRE 1

/* ---------------- segment prefix: on with the prefixed handler of the next opcode, the base in a2 ---------------- */
    .balign 4
x_26:
x_2e:
x_36:
x_3e:
    lbu t0, 0(rPC)
    srli t0, t0, 1
    andi t0, t0, 12
    add t0, t0, rA
    lw rOVB, F16A_BASE(t0)
    addi rPC, rPC, 1
    lbu t0, 0(rPC)
    slli t0, t0, 4
    add rR, t0, rT
    lw t0, REC_HOV(rR)
    jr t0

/* ---------------- Jcc rel8: to 1f when the condition holds ---------------- */
/* t = SF ^ OF in bit 31 */
.macro SXO t
    slli \t, rLV, 1
    xor \t, \t, rLV
    xor \t, \t, rLR
.endm
.macro JCC lbl, op
    .balign 4
\lbl:
    lbu t6, REC_CR(rR)
    .set cc_, ((\op) & 15)
  .if cc_ == 0
    slli t0, rLV, 1
    xor t0, t0, rLV
    bltz t0, 1f
  .elseif cc_ == 1
    slli t0, rLV, 1
    xor t0, t0, rLV
    bgez t0, 1f
  .elseif cc_ == 2
    bltz rLV, 1f
  .elseif cc_ == 3
    bgez rLV, 1f
  .elseif cc_ == 4
    beqz rLR, 1f
  .elseif cc_ == 5
    bnez rLR, 1f
  .elseif cc_ == 6
    bltz rLV, 1f
    beqz rLR, 1f
  .elseif cc_ == 7
    bltz rLV, 3f
    bnez rLR, 1f
  .elseif cc_ == 8
    bltz rLR, 1f
  .elseif cc_ == 9
    bgez rLR, 1f
  .elseif cc_ == 10
    PFB t0
    bnez t0, 1f
  .elseif cc_ == 11
    PFB t0
    beqz t0, 1f
  .elseif cc_ == 12
    SXO t0
    bltz t0, 1f
  .elseif cc_ == 13
    SXO t0
    bgez t0, 1f
  .elseif cc_ == 14
    beqz rLR, 1f
    SXO t0
    bltz t0, 1f
  .else
    beqz rLR, 3f
    SXO t0
    bgez t0, 1f
  .endif
3:  RETIRE 2
1:  lb t1, 1(rPC)
    beqz t1, 2f
    lw t2, F16A_MCKT(rA)
    lbu t2, \op(t2)
    add t6, t6, t2
2:  JREL 2, t1, t2
    RETIRE 0
.endm
    JCC x_70, 0x70
    JCC x_71, 0x71
    JCC x_72, 0x72
    JCC x_73, 0x73
    JCC x_74, 0x74
    JCC x_75, 0x75
    JCC x_76, 0x76
    JCC x_77, 0x77
    JCC x_78, 0x78
    JCC x_79, 0x79
    JCC x_7a, 0x7a
    JCC x_7b, 0x7b
    JCC x_7c, 0x7c
    JCC x_7d, 0x7d
    JCC x_7e, 0x7e
    JCC x_7f, 0x7f

/* ---------------- TEST AL / AX, imm ---------------- */
    .balign 4
x_a8:
    lbu t1, F16A_R(rA)
    lbu t2, 1(rPC)
    lbu t6, REC_CR(rR)
    and t3, t1, t2
    FLOG 8, t3
    RETIRE 2
    .balign 4
x_a9:
    lhu t1, F16A_R(rA)
    lbu t2, 1(rPC)
    lbu t3, 2(rPC)
    lbu t6, REC_CR(rR)
    slli t3, t3, 8
    or t2, t2, t3
    and t3, t1, t2
    FLOG 16, t3
    RETIRE 3

/* ---------------- MOV r, imm; XCHG AX, r ---------------- */
.macro MOVI8 lbl, i
    .balign 4
\lbl:
    lbu t1, 1(rPC)
    lbu t6, REC_CR(rR)
    sb t1, F16A_R+(((\i & 3) << 1) | (\i >> 2))(rA)
    RETIRE 2
.endm
.macro MOVI16 lbl, i
    .balign 4
\lbl:
    lbu t1, 1(rPC)
    lbu t2, 2(rPC)
    lbu t6, REC_CR(rR)
    slli t2, t2, 8
    or t1, t1, t2
    sh t1, F16A_R+2*\i(rA)
    RETIRE 3
.endm
.macro XCHG lbl, i
    .balign 4
\lbl:
    lhu t1, F16A_R(rA)
    lhu t2, F16A_R+2*\i(rA)
    lbu t6, REC_CR(rR)
    sh t2, F16A_R(rA)
    sh t1, F16A_R+2*\i(rA)
    RETIRE 1
.endm
  .irp i, 0, 1, 2, 3, 4, 5, 6, 7
    MOVI8 x_b\i, \i
    XCHG x_9\i, \i
  .endr
    MOVI16 x_b8, 0
    MOVI16 x_b9, 1
    MOVI16 x_ba, 2
    MOVI16 x_bb, 3
    MOVI16 x_bc, 4
    MOVI16 x_bd, 5
    MOVI16 x_be, 6
    MOVI16 x_bf, 7

/* ---------------- CBW CWD SAHF LAHF, flag bits ---------------- */
    .balign 4
x_98:
    lb t1, F16A_R(rA)
    lbu t6, REC_CR(rR)
    sh t1, F16A_R(rA)
    RETIRE 1
    .balign 4
x_99:
    lh t1, F16A_R(rA)
    lbu t6, REC_CR(rR)
    srai t1, t1, 31
    sh t1, F16A_R+4(rA)
    RETIRE 1
/* SAHF: SF ZF AF PF CF from AH, OF kept; an AH without an LR is handed back */
    .balign 4
x_9e:
    lbu t1, F16A_R+1(rA)
    LAZYLR t1, 98f, t0
    slli t2, rLV, 1
    xor t2, t2, rLV
    srli t2, t2, 31                    /* OF */
    xor t2, t2, t1                     /* bit 0: OF ^ CF */
    slli t2, t2, 31
    srli t2, t2, 1
    slli t3, t1, 31
    or t2, t2, t3
    srli t3, t1, 4
    andi t3, t3, 1
    slli t3, t3, 27
    or t2, t2, t3
    ori rLV, t2, 3
    lbu t6, REC_CR(rR)
    RETIRE 1
    .balign 4
x_9f:
    MATFL t1, t2
    andi t1, t1, 0xd5
    ori t1, t1, 2
    lbu t6, REC_CR(rR)
    sb t1, F16A_R+1(rA)
    RETIRE 1
    .balign 4
x_f5:
    lui t1, 0xc0000
    lbu t6, REC_CR(rR)
    xor rLV, rLV, t1
    RETIRE 1
    .balign 4
x_f8:
    srai t1, rLV, 31
    lbu t6, REC_CR(rR)
    slli t1, t1, 30
    xor rLV, rLV, t1
    RETIRE 1
    .balign 4
x_f9:
    srai t1, rLV, 31
    not t1, t1
    lbu t6, REC_CR(rR)
    slli t1, t1, 30
    xor rLV, rLV, t1
    RETIRE 1
    .balign 4
x_fa:
    andi rFS, rFS, ~0x200
    lbu t6, REC_CR(rR)
    RETIRE 1
    .balign 4
x_fc:
    andi rFS, rFS, ~0x400
    lbu t6, REC_CR(rR)
    RETIRE 1
    .balign 4
x_fd:
    ori rFS, rFS, 0x400
    lbu t6, REC_CR(rR)
    RETIRE 1

/* ---------------- JMP short / near, CALL near, RET near ---------------- */
    .balign 4
x_eb:
    lb t1, 1(rPC)
    lbu t6, REC_CR(rR)
    JREL 2, t1, t2
    RETIRE 0
    .balign 4
x_e9:
    lbu t1, 1(rPC)
    lbu t2, 2(rPC)
    lbu t6, REC_CR(rR)
    slli t2, t2, 8
    or t1, t1, t2
    JREL 3, t1, t2
    RETIRE 0
    .balign 4
x_e8:
    PUSHCHK
    lbu t3, 1(rPC)                     /* the displacement first: the push may overwrite it (stack over the code) */
    lbu t4, 2(rPC)
    slli t4, t4, 8
    or t3, t3, t4
    sub t4, rPC, rPCB
    addi t4, t4, 3
    and t4, t4, kFFFF                  /* return ip */
    add t3, t3, t4
    and t3, t3, kFFFF
    add rPC, rPCB, t3
    STW t1, t4, t0
    sh t2, F16A_R+8(rA)
    lbu t6, REC_CR(rR)
    RETIRE 0
    .balign 4
x_c3:
    POPCHK
    LDW t3, t1, t4
    addi t0, t0, 2
    sh t0, F16A_R+8(rA)
    lbu t6, REC_CR(rR)
    add rPC, rPCB, t3
    RETIRE 0
/* C2: RET imm16 */
    .balign 4
x_c2:
    POPCHK
    LDW t3, t1, t4
    lbu t1, 1(rPC)
    lbu t4, 2(rPC)
    slli t4, t4, 8
    or t1, t1, t4
    addi t0, t0, 2
    add t0, t0, t1
    sh t0, F16A_R+8(rA)
    lbu t6, REC_CR(rR)
    add rPC, rPCB, t3
    RETIRE 0

/* ---------------- E0 LOOPNZ  E1 LOOPZ  E2 LOOP  E3 JCXZ ---------------- */
.macro LOOPX lbl, n
    .balign 4
\lbl:
    lbu t6, REC_CR(rR)
    lhu t1, F16A_R+2(rA)
  .if \n == 2
    /* LOOP $ (a delay loop onto itself): the taken iterations that fit the budget in one go; the last one
     * (CX 1 -> 0) runs normally (as fast16_mini.inc m_loop) */
    lbu t0, 1(rPC)
    li t2, 0xfe
    bne t0, t2, 4f
    li t2, 1
    beq t1, t2, 4f
    blez rREM, 4f
    lw t2, F16A_MCKT(rA)
    lbu t2, 0xe2(t2)
    add t2, t2, t6                     /* clocks of one taken iteration */
    addi t0, t1, -1
    and t0, t0, kFFFF                  /* CX - 1 (CX = 0: FFFFh) */
    divu t3, rREM, t2
    bgeu t3, t0, 5f
    mv t0, t3
    bnez t0, 5f
    li t0, 1
5:  sub t1, t1, t0
    sh t1, F16A_R+2(rA)
    add rN, rN, t0
    addi rN, rN, -1
    mul t6, t0, t2
    RETIRE 0
4:
  .endif
  .if \n == 3
    bnez t1, 3f
  .else
    addi t1, t1, -1
    and t1, t1, kFFFF
    sh t1, F16A_R+2(rA)
    beqz t1, 3f
    .if \n == 0
    beqz rLR, 3f
    .elseif \n == 1
    bnez rLR, 3f
    .endif
  .endif
    lb t1, 1(rPC)
    beqz t1, 2f
    lw t2, F16A_MCKT(rA)
    lbu t2, 0xe0+\n(t2)
    add t6, t6, t2
2:  JREL 2, t1, t2
    RETIRE 0
3:  RETIRE 2
.endm
    LOOPX x_e0, 0
    LOOPX x_e1, 1
    LOOPX x_e2, 2
    LOOPX x_e3, 3

/* ---------------- LEA ---------------- */
    .balign 4
x_8d:
    lbu t5, 1(rPC)
    bgeu t5, kC0, 98f
    slli t0, t5, 2
    add t0, t0, kEAT
    lw t0, 0(t0)
    jalr ra, t0
    REGW t3, t5
    sh t0, F16A_R(t3)
    lbu t6, REC_CM(rR)
    RETIRER t2

/* ---------------- plain graphics VRAM byte moves (as fast16_mini.inc M_GVRAM / M_GVRD8 / M_GVWR8) ----------------
 * t1 linear address: hand back (island d) unless it is plain graphics VRAM (A8000h-BFFFFh, no GRCG / EGC) */
.macro GVCHK d
    lw t3, F16A_SLICEP(rA)
    BX1 beqz, t3, \d
    li t3, 0xa8000
    sub t3, t1, t3
    li t4, 0x18000
    BX bgeu, t3, t4, \d
    la t3, g_gvram_plain
    lw t3, 0(t3)
    BX1 bltz, t3, \d
.endm
/* call fn (a0 = t1 address, a1 = value set by the caller) with the clock handed over and taken back; t2 (length)
 * and t5 (ModR/M) kept; out t1 = return value, t6 = memory-form clock, constants reloaded */
.macro GVCALL fn
    sw t2, FS_T1(sp)
    sw t5, FS_T3(sp)
    lw t3, F16A_SLICEP(rA)
    lw t4, F16A_MOOFF(rA)
    add t4, rREM, t4
    sw t4, 0(t3)
    mv a0, t1
    call \fn
    mv t1, a0
    lw t3, F16A_SLICEP(rA)
    lw t3, 0(t3)
    lw t4, F16A_MOOFF(rA)
    sub rREM, t3, t4
    lw t2, FS_T1(sp)
    lw t5, FS_T3(sp)
    lbu t6, REC_CM(rR)
    KLOAD
.endm

/* ---------------- 88-8B MOV ---------------- */
/* 88: MOV r/m8, r8. A write at A0000h or above: plain graphics VRAM through the bus handler, the rest handed back */
.macro MOV88 lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
    slli t0, t5, 4
    add t0, t0, rT
    lbu t1, REC_REGB(t0)
    lbu t0, REC_RMB(t0)
    add t1, t1, rA
    add t0, t0, rA
    lbu t2, F16A_R(t1)
    lbu t6, REC_CR(rR)
    sb t2, F16A_R(t0)
    RETIRE 2
.Lm\@:
    EA
    bgeu t1, kWEND, .Ls\@
    add t1, t1, rRAM
    REGB t3, t5
    lbu t3, F16A_R(t3)
    lbu t6, REC_CM(rR)
    STB t1, t3
    RETIRER t2
.Ls\@:
    GVCHK b
    REGB t3, t5
    lbu a1, F16A_R(t3)
    GVCALL gvram_plain_wr8
    RETIRER t2
    .set xov, 0
.endm
.macro MOV89 lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
    RMW t0, t5
    REGW t1, t5
    lhu t2, F16A_R(t1)
    lbu t6, REC_CR(rR)
    sh t2, F16A_R(t0)
    RETIRE 2
.Lm\@:
    EA
    CHKW 0, b
    add t1, t1, rRAM
    REGW t3, t5
    lhu t3, F16A_R(t3)
    lbu t6, REC_CM(rR)
    STW t1, t3, t0
    RETIRER t2
    .set xov, 0
.endm
/* 8A: MOV r8, r/m8. A read at A4000h or above: as 88 */
.macro MOV8A lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
    slli t0, t5, 4
    add t0, t0, rT
    lbu t1, REC_REGB(t0)
    lbu t0, REC_RMB(t0)
    add t1, t1, rA
    add t0, t0, rA
    lbu t2, F16A_R(t0)
    lbu t6, REC_CR(rR)
    sb t2, F16A_R(t1)
    RETIRE 2
.Lm\@:
    EA
    bgeu t1, kREND, .Ls\@
    add t1, t1, rRAM
    lbu t1, 0(t1)
    REGB t3, t5
    lbu t6, REC_CM(rR)
    sb t1, F16A_R(t3)
    RETIRER t2
.Ls\@:
    GVCHK b
    GVCALL gvram_plain_rd8
    REGB t3, t5
    sb t1, F16A_R(t3)
    RETIRER t2
    .set xov, 0
.endm
.macro MOV8B lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
    RMW t0, t5
    REGW t1, t5
    lhu t2, F16A_R(t0)
    lbu t6, REC_CR(rR)
    sh t2, F16A_R(t1)
    RETIRE 2
.Lm\@:
    EA
    CHKW 1, b
    add t1, t1, rRAM
    LDW t3, t1, t4
    REGW t1, t5
    lbu t6, REC_CM(rR)
    sh t3, F16A_R(t1)
    RETIRER t2
    .set xov, 0
.endm
    MOV88 x_88, 0
    MOV88 xo_88, 1
    MOV89 x_89, 0
    MOV89 xo_89, 1
    MOV8A x_8a, 0
    MOV8A xo_8a, 1
    MOV8B x_8b, 0
    MOV8B xo_8b, 1

/* ---------------- 84 85 TEST r/m, r ---------------- */
.macro TESTRM lbl, w, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
  .if \w == 16
    RMW t0, t5
    REGW t1, t5
    lhu t2, F16A_R(t0)
    lhu t3, F16A_R(t1)
  .else
    slli t0, t5, 4
    add t0, t0, rT
    lbu t1, REC_REGB(t0)
    lbu t0, REC_RMB(t0)
    add t1, t1, rA
    add t0, t0, rA
    lbu t2, F16A_R(t0)
    lbu t3, F16A_R(t1)
  .endif
    lbu t6, REC_CR(rR)
    and t2, t2, t3
    FLOG \w, t2
    RETIRE 2
.Lm\@:
    EA
  .if \w == 16
    CHKW 1, b
    add t1, t1, rRAM
    LDW t3, t1, t4
    REGW t4, t5
    lhu t4, F16A_R(t4)
  .else
    CHKB 1, b
    add t1, t1, rRAM
    lbu t3, 0(t1)
    REGB t4, t5
    lbu t4, F16A_R(t4)
  .endif
    lbu t6, REC_CM(rR)
    and t3, t3, t4
    FLOG \w, t3
    RETIRER t2
    .set xov, 0
.endm
    TESTRM x_84, 8, 0
    TESTRM xo_84, 8, 1
    TESTRM x_85, 16, 0
    TESTRM xo_85, 16, 1

/* ---------------- 80-83: ALU r/m, imm ----------------
 * t1 = pointer to the operand (RAM, or the register inside A as a byte pointer), t2 = its value, t3 = imm,
 * t6 = clock (mck_main by operation), pc past the instruction; then by the reg field through JT8 */
.macro GRP1 lbl, op, ov
    .set xov, \ov
    .set wd_, 8+8*((\op) & 1)
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    bltu t5, kC0, .Lm\@
  .if wd_ == 16
    RMW t1, t5
    lhu t2, F16A_R(t1)
  .else
    RMB t1, t5
    lbu t2, F16A_R(t1)
  .endif
    srli t0, t5, 3
    andi t0, t0, 7
    MAINCLK t6, \op, t0, 0, t3
  .if (\op) == 0x81
    lbu t3, 2(rPC)
    lbu t4, 3(rPC)
    addi rPC, rPC, 4
    slli t4, t4, 8
    or t3, t3, t4
  .elseif (\op) == 0x83
    lb t3, 2(rPC)
    addi rPC, rPC, 3
    and t3, t3, kFFFF
  .else
    lbu t3, 2(rPC)
    addi rPC, rPC, 3
  .endif
    JT8 t0, t4, \lbl\()_rops
  .irp k, 0, 1, 2, 3, 4, 5, 6
2\k:
    ALUOP \k, wd_, t4, t2, t3, a0
    .if wd_ == 16
    sh t4, F16A_R(t1)
    .else
    sb t4, F16A_R(t1)
    .endif
    RETIRE 0
  .endr
27:
    ALUOP 7, wd_, t4, t2, t3, a0
    RETIRE 0
.Lm\@:
    EA
  .if wd_ == 16
    CHKW 0, b
  .else
    CHKB 0, b
  .endif
    add t1, t1, rRAM
    srli t0, t5, 3
    andi t0, t0, 7
    MAINCLK t6, \op, t0, 1, t3
    add t4, rPC, t2                    /* the immediate */
  .if wd_ == 8
    lbu t2, 0(t1)
    lbu t3, 0(t4)
    addi rPC, t4, 1
  .elseif (\op) == 0x83
    LDW t2, t1, t3
    lb t3, 0(t4)
    addi rPC, t4, 1
    and t3, t3, kFFFF
  .else
    LDW t2, t1, t3
    lbu t3, 0(t4)
    lbu a0, 1(t4)
    addi rPC, t4, 2
    slli a0, a0, 8
    or t3, t3, a0
  .endif
    JT8 t0, t4, \lbl\()_mops
  .irp k, 0, 1, 2, 3, 4, 5, 6
2\k:
    ALUOP \k, wd_, t4, t2, t3, a0
    .if wd_ == 16
    STW t1, t4, a0
    .else
    STB t1, t4
    .endif
    RETIRE 0
  .endr
27:
    ALUOP 7, wd_, t4, t2, t3, a0
    RETIRE 0
    .set xov, 0
.endm
    GRP1 x_80, 0x80, 0
    GRP1 xo_80, 0x80, 1
    GRP1 x_81, 0x81, 0
    GRP1 xo_81, 0x81, 1
    GRP1 x_82, 0x82, 0
    GRP1 xo_82, 0x82, 1
    GRP1 x_83, 0x83, 0
    GRP1 xo_83, 0x83, 1

/* ---------------- A0-A3: accumulator <-> [moffs] ---------------- */
.macro MOVA lbl, op, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t0, 1(rPC)
    lbu t1, 2(rPC)
    slli t1, t1, 8
    or t0, t0, t1
  .if \ov
    add t1, rOVB, t0
  .else
    lw t1, F16A_BASE+12(rA)
    add t1, t1, t0
  .endif
    lbu t6, REC_CR(rR)
  .if (\op) == 0xa0
    CHKB 1, f
    add t1, t1, rRAM
    lbu t2, 0(t1)
    sb t2, F16A_R(rA)
  .elseif (\op) == 0xa1
    CHKW 1, f
    add t1, t1, rRAM
    LDW t2, t1, t3
    sh t2, F16A_R(rA)
  .elseif (\op) == 0xa2
    CHKB 0, f
    add t1, t1, rRAM
    lbu t2, F16A_R(rA)
    STB t1, t2
  .else
    CHKW 0, f
    add t1, t1, rRAM
    lhu t2, F16A_R(rA)
    STW t1, t2, t3
  .endif
    RETIRE 3
    .set xov, 0
.endm
    MOVA x_a0, 0xa0, 0
    MOVA xo_a0, 0xa0, 1
    MOVA x_a1, 0xa1, 0
    MOVA xo_a1, 0xa1, 1
    MOVA x_a2, 0xa2, 0
    MOVA xo_a2, 0xa2, 1
    MOVA x_a3, 0xa3, 0
    MOVA xo_a3, 0xa3, 1

/* ---------------- C6 C7: MOV r/m, imm (reg field 0) ---------------- */
.macro MOVIM lbl, w, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    andi t0, t5, 0x38
    BX1 bnez, t0, f
    bltu t5, kC0, .Lm\@
  .if \w == 16
    RMW t1, t5
    lbu t2, 2(rPC)
    lbu t3, 3(rPC)
    lbu t6, REC_CR(rR)
    slli t3, t3, 8
    or t2, t2, t3
    sh t2, F16A_R(t1)
    RETIRE 4
  .else
    RMB t1, t5
    lbu t2, 2(rPC)
    lbu t6, REC_CR(rR)
    sb t2, F16A_R(t1)
    RETIRE 3
  .endif
.Lm\@:
    EA
  .if \w == 16
    CHKW 0, b
  .else
    CHKB 0, b
  .endif
    add t1, t1, rRAM
    add t3, rPC, t2
    lbu t4, 0(t3)
  .if \w == 16
    lbu t3, 1(t3)
    slli t3, t3, 8
    or t4, t4, t3
    STW t1, t4, t3
    addi t2, t2, 2
  .else
    STB t1, t4
    addi t2, t2, 1
  .endif
    lbu t6, REC_CM(rR)
    RETIRER t2
    .set xov, 0
.endm
    MOVIM x_c6, 8, 0
    MOVIM xo_c6, 8, 1
    MOVIM x_c7, 16, 0
    MOVIM xo_c7, 16, 1

/* ---------------- 8C: MOV r/m16, sreg (sreg 0..3) ---------------- */
.macro MOV8C lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    andi t0, t5, 0x20
    BX1 bnez, t0, f
    srli t3, t5, 2
    andi t3, t3, 6
    add t3, t3, rA
    lhu t3, F16A_SEG(t3)
    bltu t5, kC0, .Lm\@
    RMW t1, t5
    lbu t6, REC_CR(rR)
    sh t3, F16A_R(t1)
    RETIRE 2
.Lm\@:
    mv t4, t3
    EA
    CHKW 0, b
    add t1, t1, rRAM
    lbu t6, REC_CM(rR)
    STW t1, t4, t0
    RETIRER t2
    .set xov, 0
.endm
    MOV8C x_8c, 0
    MOV8C xo_8c, 1

/* ---------------- 8E: MOV sreg, r/m16 (ES or DS); the segment cache as M_LOADSEG ---------------- */
.macro MOV8E lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    srli t4, t5, 3
    andi t4, t4, 7
    beqz t4, 1f
    li t0, 3
    BX bne, t4, t0, f
1:  bltu t5, kC0, .Lm\@
    RMW t0, t5
    lhu t3, F16A_R(t0)
    li t2, 2
    MAINCLK t6, 0x8e, t4, 0, t0
    j .Ld\@
    ISLAND
.Lm\@:
    EA
    CHKW 1, b
    add t1, t1, rRAM
    LDW t3, t1, t0
    MAINCLK t6, 0x8e, t4, 1, t0
.Ld\@:
    add rPC, rPC, t2
    LOADSEG t4, t3, t0, t1
    RETIRE 0
    .set xov, 0
.endm
    MOV8E x_8e, 0
    MOV8E xo_8e, 1

/* ---------------- C4 C5: LES LDS (as the generic loop: offset at most FFFCh, the four bytes below A4000h) ---------------- */
.macro LXS lbl, sg, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    BX bgeu, t5, kC0, f
    EA
    li t3, 0xfffc
    BX bltu, t3, t0, f
    li t3, 0xa3ffc
    BX bltu, t3, t1, f
    add t1, t1, rRAM
    lbu t6, REC_CM(rR)
    add rPC, rPC, t2
    LDW t3, t1, t4
    lbu t0, 2(t1)
    lbu t4, 3(t1)
    slli t4, t4, 8
    or t0, t0, t4
    REGW t4, t5
    sh t3, F16A_R(t4)
    LOADSEGK \sg, t0, t1, t4
    RETIRE 0
    .set xov, 0
.endm
    LXS x_c4, 0, 0
    LXS xo_c4, 0, 1
    LXS x_c5, 3, 0
    LXS xo_c5, 3, 1

/* ---------------- far transfers ---------------- */
/* push CS, then ret (register), as the generic loop's PUSHW pair; to the island d unless the four bytes are in RAM
 * below A0000h (SP 1..3: the generic loop). t u v scratch */
.macro FARPUSH ret, t, u, v, d
    lhu \t, F16A_R+8(rA)
    beqz \t, .Lok\@
    sltiu \u, \t, 4
    BX1 bnez, \u, \d
.Lok\@:
    addi \t, \t, -4
    and \t, \t, kFFFF
    lw \u, F16A_BASE+8(rA)
    add \u, \u, \t
    addi \v, \u, 3
    BX bgeu, \v, kWEND, \d
    add \u, \u, rRAM
    sh \t, F16A_R+8(rA)
    lhu \v, F16A_SEG+2(rA)
    addi \u, \u, 2
    STW \u, \v, \t
    addi \u, \u, -2
    STW \u, \ret, \v
.endm
/* CS = t3 (selector), IP = t4, t6 clocks: retire. A target in RAM below A4000h stays in the loop with its code
 * window (M_CSWIN); any other one (ROM, the ITF bank, HMA) ends the batch and the generic loop sets the window. */
.macro FARGO
    LOADSEGK 1, t3, t0, t1, 0x9b
    slli t3, t3, 4
    add rPCB, rRAM, t3
    add rPC, rPCB, t4
    li t1, 0xa3ff6
    bltu t1, t3, .Lx\@
    sw rPCB, F16A_MOPCB(rA)
    sw zero, F16A_MOROM(rA)
    li t1, 0xa3ff7
    sub t1, t1, t3
    li t2, 0xfff0
    bltu t1, t2, 1f
    mv t1, t2
1:  add rLIM, rPCB, t1
    RETIRE 0
.Lx\@:
    sub rREM, rREM, t6
    addi rN, rN, 1
    TRACE
    j f16x_exit
.endm
/* EA: JMP far ptr16:16 */
    .balign 4
x_ea:
    lbu t6, REC_CR(rR)
    lbu t4, 1(rPC)
    lbu t0, 2(rPC)
    lbu t3, 3(rPC)
    lbu t1, 4(rPC)
    slli t0, t0, 8
    or t4, t4, t0
    slli t1, t1, 8
    or t3, t3, t1
    FARGO
/* 9A: CALL far ptr16:16 (the pointer read before the pushes: the stack may lie over the code) */
    .balign 4
x_9a:
    lbu t6, REC_CR(rR)
    lbu t4, 1(rPC)
    lbu t0, 2(rPC)
    lbu t3, 3(rPC)
    lbu t1, 4(rPC)
    slli t0, t0, 8
    or t4, t4, t0
    slli t1, t1, 8
    or t3, t3, t1
    sub a0, rPC, rPCB
    addi a0, a0, 5
    and a0, a0, kFFFF
    FARPUSH a0, t0, t1, t2, f
    FARGO
/* CA CB: RETF (imm16) */
.macro RETF lbl, imm
    .balign 4
\lbl:
    lbu t6, REC_CR(rR)
    lhu t0, F16A_R+8(rA)
    li t1, 0xfffc
    bltu t1, t0, 98f
    lw t1, F16A_BASE+8(rA)
    add t1, t1, t0
    li t2, 0xa3ffc
    bltu t2, t1, 98f
    add t1, t1, rRAM
  .if \imm
    lbu t2, 1(rPC)
    lbu t3, 2(rPC)
    slli t3, t3, 8
    or t2, t2, t3
    add t0, t0, t2
  .endif
    addi t0, t0, 4
    sh t0, F16A_R+8(rA)
    LDW t4, t1, t2
    lbu t3, 2(t1)
    lbu t2, 3(t1)
    slli t2, t2, 8
    or t3, t3, t2
    FARGO
.endm
    RETF x_ca, 1
    RETF x_cb, 0

/* ---------------- FE: INC / DEC r/m8 ---------------- */
.macro GRPFE lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    andi t0, t5, 0x30
    BX1 bnez, t0, f
    bltu t5, kC0, .Lm\@
    RMP t1, t5, 8
    li t2, 2
    lbu t6, REC_CR(rR)
    j .Lc\@
    ISLAND
.Lm\@:
    EA
    CHKB 0, b
    add t1, t1, rRAM
    lbu t6, REC_CM(rR)
.Lc\@:
    add rPC, rPC, t2
    lbu t3, 0(t1)
    andi t0, t5, 8
    bnez t0, 1f
    addi t4, t3, 1
    j 2f
1:  addi t4, t3, -1
2:  STB t1, t4
    FLID 8, t3, t4, t0, a0
    RETIRE 0
    .set xov, 0
.endm
    GRPFE x_fe, 0
    GRPFE xo_fe, 1

/* ---------------- FF: INC DEC CALL JMP PUSH, CALL far / JMP far (memory) on r/m16 ---------------- */
.macro GRPFF lbl, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    srli t4, t5, 3
    andi t4, t4, 7
    li t0, 7
    BX beq, t4, t0, f
    bltu t5, kC0, .Lm\@
    li t0, 3
    BX beq, t4, t0, f
    li t0, 5
    BX beq, t4, t0, f
    RMP t1, t5, 16
    li t2, 2
    MAINCLK t6, 0xff, t4, 0, t0
    j .Lc\@
    ISLAND
.Lm\@:
    EA
    li t3, 3
    beq t4, t3, .Lfar\@
    li t3, 5
    beq t4, t3, .Lfar\@
    srli t3, t4, 1
    bnez t3, 1f
    CHKW 0, b
    j 2f
1:  CHKW 1, b
2:  add t1, t1, rRAM
    MAINCLK t6, 0xff, t4, 1, t0
.Lc\@:
    LDW t3, t1, t0                     /* operand */
    beqz t4, .Linc\@
    li t0, 1
    beq t4, t0, .Ldec\@
    li t0, 2
    beq t4, t0, .Lcall\@
    li t0, 4
    beq t4, t0, .Ljmp\@
    /* 6: PUSH */
    lhu t0, F16A_R+8(rA)
    addi a0, t0, -1
    BX1 beqz, a0, f
    addi t0, t0, -2
    and t0, t0, kFFFF
    lw a0, F16A_BASE+8(rA)
    add a0, a0, t0
    addi t4, a0, 1
    BX bgeu, t4, kWEND, f
    add a0, a0, rRAM
    STW a0, t3, t4
    sh t0, F16A_R+8(rA)
    add rPC, rPC, t2
    RETIRE 0
.Linc\@:
    addi t4, t3, 1
    j 1f
.Ldec\@:
    addi t4, t3, -1
1:  STW t1, t4, t0
    add rPC, rPC, t2
    FLID 16, t3, t4, t0, a0
    RETIRE 0
.Ljmp\@:
    add rPC, rPCB, t3
    RETIRE 0
.Lcall\@:
    lhu t0, F16A_R+8(rA)
    addi a0, t0, -1
    BX1 beqz, a0, b
    addi t0, t0, -2
    and t0, t0, kFFFF
    lw a0, F16A_BASE+8(rA)
    add a0, a0, t0
    addi t4, a0, 1
    BX bgeu, t4, kWEND, b
    add a0, a0, rRAM
    sh t0, F16A_R+8(rA)
    sub t4, rPC, rPCB
    add t4, t4, t2
    and t4, t4, kFFFF                  /* return ip */
    add rPC, rPCB, t3
    STW a0, t4, t0
    RETIRE 0
.Lfar\@:                               /* 3 5: the pointer at the offset (at most FFFCh, below A4000h) */
    li t3, 0xfffc
    BX bltu, t3, t0, b
    li t3, 0xa3ffc
    BX bltu, t3, t1, b
    add t1, t1, rRAM
    MAINCLK t6, 0xff, t4, 1, t0
    LDW a0, t1, t3                     /* IP */
    lbu t3, 2(t1)                      /* CS */
    lbu t1, 3(t1)
    slli t1, t1, 8
    or t3, t3, t1
    li t1, 3
    bne t4, t1, 3f
    sub t0, rPC, rPCB                  /* 3: CALL far, the return address pushed */
    add t0, t0, t2
    and t0, t0, kFFFF
    FARPUSH t0, t1, t2, t4, b
3:  mv t4, a0
    FARGO
    .set xov, 0
.endm
    GRPFF x_ff, 0
    GRPFF xo_ff, 1

/* ---------------- F6 / F7: TEST NOT NEG ---------------- */
.macro GRP3 lbl, op, B, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    srli t4, t5, 3
    andi t4, t4, 7
    li t0, 0xf2
    srl t0, t0, t4
    andi t0, t0, 1
    BX1 bnez, t0, f                    /* 1 4 5 6 7: generic loop */
    bltu t5, kC0, .Lm\@
    RMP t1, t5, \B
    li t2, 2
    MAINCLK t6, \op, t4, 0, t0
    j .Lc\@
    ISLAND
.Lm\@:
    EA
    andi t3, t5, 0x10
    bnez t3, 1f
  .if \B == 16
    CHKW 1, b
  .else
    CHKB 1, b
  .endif
    j 2f
1:
  .if \B == 16
    CHKW 0, b
  .else
    CHKB 0, b
  .endif
2:  add t1, t1, rRAM
    MAINCLK t6, \op, t4, 1, t0
.Lc\@:
  .if \B == 16
    LDW t3, t1, t0
  .else
    lbu t3, 0(t1)
  .endif
    add rPC, rPC, t2
    beqz t4, 3f
    andi t0, t4, 1
    bnez t0, 4f
    /* 2: NOT */
    not t0, t3
  .if \B == 16
    STW t1, t0, a0
  .else
    STB t1, t0
  .endif
    RETIRE 0
4:  /* 3: NEG */
    ALUOP 5, \B, t0, zero, t3, a0
  .if \B == 16
    STW t1, t0, a0
  .else
    STB t1, t0
  .endif
    RETIRE 0
3:  /* 0: TEST imm */
    lbu t0, 0(rPC)
  .if \B == 16
    lbu a0, 1(rPC)
    slli a0, a0, 8
    or t0, t0, a0
    addi rPC, rPC, 2
  .else
    addi rPC, rPC, 1
  .endif
    and t0, t0, t3
    FLOG \B, t0
    RETIRE 0
    .set xov, 0
.endm
    GRP3 x_f6, 0xf6, 8, 0
    GRP3 xo_f6, 0xf6, 8, 1
    GRP3 x_f7, 0xf7, 16, 0
    GRP3 xo_f7, 0xf7, 16, 1

/* ---------------- D0 / D1: shift / rotate by 1 ---------------- */
/* t0 = result (unmasked) from v = t3, the flags; t4 a0 t2 scratch */
.macro SH1 op, B
  .if \op == 0                         /* ROL: CF = new bit 0, OF = CF ^ new msb */
    slli t0, t3, 1
    srli t4, t3, \B-1
    or t0, t0, t4
    andi t4, t0, 1
    srli a0, t0, \B-1
    andi a0, a0, 1
    ROTFL t4, a0, t2, 0
  .elseif \op == 1                     /* ROR: CF = new msb, OF = msb ^ next */
    srli t0, t3, 1
    slli t4, t3, \B-1
    or t0, t0, t4
    andi t4, t3, 1
    srli a0, t3, \B-1
    andi a0, a0, 1
    ROTFL t4, a0, t2, 0
  .elseif \op == 2                     /* RCL */
    srli t4, rLV, 31
    slli t0, t3, 1
    or t0, t0, t4
    srli t4, t3, \B-1
    andi t4, t4, 1
    srli a0, t3, \B-2
    andi a0, a0, 1
    ROTFL t4, a0, t2, 0
  .elseif \op == 3                     /* RCR: OF = old CF ^ old msb */
    srli t4, rLV, 31
    slli a0, t4, \B-1
    srli t0, t3, 1
    or t0, t0, a0
    srli a0, t3, \B-1
    andi a0, a0, 1
    xor a0, a0, t4
    andi t4, t3, 1
    xor a0, a0, t4
    ROTFL t4, a0, t2, 0
  .elseif \op == 4                     /* SHL */
    slli t0, t3, 1
    srli t4, t3, \B-1
    andi t4, t4, 1
    srli a0, t3, \B-2
    andi a0, a0, 1
    ROTFL t4, a0, t2, \B
    slli rLR, t0, 32-\B
  .elseif \op == 5                     /* SHR: OF = old msb */
    srli t0, t3, 1
    andi t4, t3, 1
    srli a0, t3, \B-1
    xor a0, a0, t4
    ROTFL t4, a0, t2, \B
    slli rLR, t0, 32-\B
  .else                                /* SAR: OF = 0 */
    srli t0, t3, 1
    srli t4, t3, \B-1
    slli t4, t4, \B-1
    or t0, t0, t4
    andi t4, t3, 1
    mv a0, t4
    ROTFL t4, a0, t2, \B
    slli rLR, t0, 32-\B
  .endif
.endm
.macro SHIFT1 lbl, op, B, ov
    .set xov, \ov
    .balign 4
\lbl:
    lbu t5, 1(rPC)
    srli t4, t5, 3
    andi t4, t4, 7
    li t0, 6
    BX beq, t4, t0, f
    bltu t5, kC0, .Lm\@
    RMP t1, t5, \B
  .if \B == 16
    lhu t3, 0(t1)
  .else
    lbu t3, 0(t1)
  .endif
    lbu t6, REC_CR(rR)
    addi rPC, rPC, 2
    JT8 t4, t0, \lbl\()_rops
  .irp k, 0, 1, 2, 3, 4, 5, 7
2\k:
    SH1 \k, \B
    .if \B == 16
    sh t0, 0(t1)
    .else
    sb t0, 0(t1)
    .endif
    RETIRE 0
  .endr
26: j f16x_exit
.Lm\@:
    EA
  .if \B == 16
    CHKW 0, b
  .else
    CHKB 0, b
  .endif
    add t1, t1, rRAM
    lbu t6, REC_CM(rR)
    add rPC, rPC, t2
  .if \B == 16
    LDW t3, t1, t0
  .else
    lbu t3, 0(t1)
  .endif
    JT8 t4, t0, \lbl\()_mops
  .irp k, 0, 1, 2, 3, 4, 5, 7
2\k:
    SH1 \k, \B
    .if \B == 16
    STW t1, t0, t2
    .else
    STB t1, t0
    .endif
    RETIRE 0
  .endr
26: j f16x_exit
    .set xov, 0
.endm
    SHIFT1 x_d0, 0xd0, 8, 0
    SHIFT1 xo_d0, 0xd0, 8, 1
    SHIFT1 x_d1, 0xd1, 16, 0
    SHIFT1 xo_d1, 0xd1, 16, 1

/* ---------------- port I/O: E4-E7 EC-EF (as fast16_mini.inc m_io; f16x_in / f16x_out publish FLAGS and the clock) ---------------- */
    .balign 4
x_e4:
x_e5:
x_e6:
x_e7:
x_ec:
x_ed:
x_ee:
x_ef:
    lbu t3, 0(rPC)
    lw t0, F16A_SLICEP(rA)
    beqz t0, 98f
    andi t1, t3, 8
    bnez t1, 1f
    lbu t4, 1(rPC)                     /* port imm8, length 2 */
    li t2, 2
    j 2f
1:  lhu t4, F16A_R+4(rA)               /* port DX, length 1 */
    li t2, 1
2:  addi t1, t4, -0xf0
    sltiu t1, t1, 8
    bnez t1, 98f                       /* F0..F7: CPU reset / A20 */
    sw t2, FS_T1(sp)
    sw t3, FS_T3(sp)
    MATFL a3, t0
    lw a4, F16A_MOOFF(rA)
    add a4, rREM, a4                   /* the clock left before this instruction, as the bus sees it */
    andi a2, t3, 1                     /* word */
    mv a1, t4
    mv a0, rA
    andi t1, t3, 2
    bnez t1, 3f
    call f16x_in                       /* f16x_in(A, port, wide, fl, remv) */
    lw t3, FS_T3(sp)
    andi t1, t3, 1
    bnez t1, 5f
    sb a0, F16A_R(rA)
    j 4f
5:  sh a0, F16A_R(rA)
    j 4f
3:  mv a5, a4                          /* f16x_out(A, port, wide, AX, fl, remv) */
    mv a4, a3
    lhu a3, F16A_R(rA)
    call f16x_out
4:  KLOAD
    lw t0, F16A_SLICEP(rA)
    lw t0, 0(t0)
    lw t1, F16A_MOOFF(rA)
    sub rREM, t0, t1                   /* devices may retarget the clock */
    lw t2, FS_T1(sp)
    lw t3, FS_T3(sp)
    lbu t6, REC_CR(rR)
    andi t1, t3, 2
    beqz t1, 6f
    lw t1, F16A_MOROM(rA)
    beqz t1, 6f
    /* a port write while running from ROM may switch the ITF bank: retire, then look at the code window again */
    sub rREM, rREM, t6
    add rPC, rPC, t2
    addi rN, rN, 1
    TRACE
    j f16x_exit
6:  RETIRER t2

    .set xov, 0
    ISLAND
    .size f16x_start, . - f16x_start

/* ---------------- tables ---------------- */
/* EA stub by ModR/M (00-BF) */
    .section .dram1.f16x_eat, "aw"
    .balign 4
    .globl f16x_eat
f16x_eat:
  .irp mod, 0, 1, 2
    .rept 8
    .word x_ea_\mod\()0, x_ea_\mod\()1, x_ea_\mod\()2, x_ea_\mod\()3, x_ea_\mod\()4, x_ea_\mod\()5, x_ea_\mod\()6, x_ea_\mod\()7
    .endr
  .endr

/* handlers by opcode (0: none here): plain, and after a segment prefix */
.macro HROW p, hi
  .irp lo, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, a, b, c, d, e, f
    .ifdef \p\hi\lo
    .word \p\hi\lo
    .else
    .word 0
    .endif
  .endr
.endm
    .section .rodata.f16x_h, "a"
    .balign 4
    .globl f16x_hdef
f16x_hdef:
  .irp hi, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, a, b, c, d, e, f
    HROW x_, \hi
  .endr
    .globl f16x_hov
f16x_hov:
  .irp hi, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, a, b, c, d, e, f
    HROW xo_, \hi
  .endr
