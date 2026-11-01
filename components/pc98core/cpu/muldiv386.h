/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_MULDIV386_H
#define PC98_CPU_MULDIV386_H
#include <stdint.h>
/* Intel 80386 PRM MUL/IMUL/DIV/IDIV, F7 /4..7, 32-bit operands.
 * Return 0 on divide error, preserving flags and both output words.
 * MUL/IMUL define CF/OF only; divide status is undefined and retained.
 * Exception delivery, operand fetch and register aliasing belong to caller. */
int pc98_cpu_muldiv386(unsigned op,uint32_t ax,uint32_t dx,uint32_t operand,uint32_t *flags,uint32_t *out_ax,uint32_t *out_dx);
#endif
