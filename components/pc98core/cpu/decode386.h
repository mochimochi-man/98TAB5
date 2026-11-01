/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_DECODE386_H
#define PC98_CPU_DECODE386_H
#include "decode.h"
/* Segment identifiers 0..3 match Pc98Seg; FS/GS are decode-only here. */
enum Pc98ExtraSegment386 {PC98_FS386=4,PC98_GS386=5};
typedef struct {unsigned reg,rm,is_register,segment,sp_base;uint32_t offset;} Pc98Operand386;
/* Intel 80386 PRM 2.5.3 and tables 17-3/17-4, 32-bit address size.
 * Code fetch still uses the supplied 16-bit execution cursor/budget; this is
 * the address-override foundation for 16-bit code, not 32-bit code execution.
 * sp_base marks an explicit ESP base (never an index), for memory POP
 * effective-address adjustment after incrementing the implicit stack pointer.
 * No operand memory access, segment translation or exception delivery occurs.
 * On fetch failure the caller must check cpu->fault and discard the operand. */
Pc98Operand386 pc98_cpu_modrm386(Pc98Cpu16State *,const Pc98FetchBus *,const uint32_t gpr[8],int override_segment);
#endif
