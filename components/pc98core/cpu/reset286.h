/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_RESET286_H
#define PC98_CPU_RESET286_H
#include "mode286.h"
/* Intel 286 PRM 10.4 reset-release architectural state, without bus or timing.
 * Reinitializes the mode in place, including shutdown and all owned pointers.
 * Undefined general/table/task register contents are deterministically zero.
 * Memory, A20, peripherals and the physical RESET line belong to the adapter. */
void pc98_cpu_reset286(Pc98Mode286 *,Pc98Cpu16State *);
#endif
