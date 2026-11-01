/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Integer arithmetic for the independent 16-bit CPU. Intel 80286 PRM,
 * sections 2.3.4, 3.3 and 3.4; NEC V20/V30 User Manual arithmetic instructions.
 * This module is host-tested only until the instruction core is complete. */
#ifndef PC98_CPU_ALU_H
#define PC98_CPU_ALU_H
#include <stdint.h>
enum Pc98AluOp { PC98_ADD, PC98_OR, PC98_ADC, PC98_SBB, PC98_AND,
 PC98_SUB, PC98_XOR, PC98_CMP, PC98_INC, PC98_DEC, PC98_NEG };
enum Pc98Flag { PC98_CF=1, PC98_PF=4, PC98_AF=16, PC98_ZF=64,
 PC98_SF=128, PC98_TF=256, PC98_IF=512, PC98_DF=1024, PC98_OF=2048 };
/* bits must be 8 or 16. CMP returns the subtraction result; callers suppress
 * writeback. Logical operations preserve undefined AF deterministically.
 * INC/DEC preserve CF. All operations preserve non-arithmetic flag bits. */
uint16_t pc98_cpu_alu(enum Pc98AluOp op, unsigned bits, uint16_t left,
                      uint16_t right, uint16_t *flags);
/* Group-2 encoding: ROL,ROR,RCL,RCR,SHL,SHR,(reserved),SAR.
 * Caller applies model-specific count masking. Undefined AF and multi-count
 * OF are preserved deterministically; op 6 is not accepted. */
uint16_t pc98_cpu_shift(unsigned op,unsigned bits,uint16_t value,
                         unsigned count,uint16_t *flags);
/* Group-3 /4..7: unsigned/signed multiply and divide. Return 0 for
 * divide error, leaving outputs and flags untouched. Undefined flags are
 * preserved; exceptions are raised by the future execution adapter. */
int pc98_cpu_muldiv(unsigned op,unsigned bits,uint16_t ax,uint16_t dx,
                    uint16_t operand,uint16_t *flags,uint16_t *out_ax,uint16_t *out_dx);
/* 286 decimal/ASCII adjustment, manufacturer-documented D4/D5 base 10.
 * opcode is 27,2f,37,3f,d4,d5. Undefined status flags are preserved. */
uint16_t pc98_cpu_adjust286(unsigned opcode,uint16_t ax,uint16_t *flags);
/* NEC native adjustments. DAA defines signed overflow of its correction;
 * AAM uses the observed native whole-AX ZF compatibility contract, pending
 * independent physical-V30 confirmation of the manual's ambiguous width. */
uint16_t pc98_cpu_adjust_v30(unsigned opcode,uint16_t ax,uint16_t *flags);
/* Generalized Intel D4/D5 forms. AAM base zero returns 0 without changes.
 * AAD accepts base zero. Result status is determined from AL per Intel SDM. */
int pc98_cpu_adjust_base286(unsigned opcode,uint16_t ax,uint8_t base,
                            uint16_t *flags,uint16_t *out);
#endif
