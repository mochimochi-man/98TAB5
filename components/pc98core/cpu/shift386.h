/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_SHIFT386_H
#define PC98_CPU_SHIFT386_H
#include <stdint.h>
/* Intel 80386 PRM 17-135..138/17-146..149: group2 /0..5,/7,
 * 32-bit operands, five-bit count mask. /6 is not accepted.
 * Zero count preserves everything; undefined multi-count OF and shift AF
 * are retained deterministically. Other unaffected flags are preserved. */
uint32_t pc98_cpu_shift386(unsigned op,uint32_t value,unsigned count,uint32_t *flags);
#endif
