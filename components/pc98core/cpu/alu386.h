/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_ALU386_H
#define PC98_CPU_ALU386_H
#include "alu.h"
/* 32-bit arithmetic/logical status, Intel 80386 PRM chapter 3.
 * CMP returns subtraction; caller suppresses writeback. INC/DEC retain CF.
 * Undefined AF of logical operations is retained deterministically. */
uint32_t pc98_cpu_alu386(enum Pc98AluOp,uint32_t left,uint32_t right,uint32_t *flags);
#endif
