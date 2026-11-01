/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_MENU_RESET_H
#define PC98_CPU_MENU_RESET_H
#include "menu_cpu.h"
/* PC-98 platform cold CPU reset, not a physical386 RESET model. Mirrors
 * machine.c reset_vector's F000:FFF0/000F0000 mapping after286 reset.
 * Caller owns A20, clocks, memory allocations and device/event reset.
 * Explicit menu remains authoritative:0 V30,1 286,2 hybrid. Undefined
 * GPRs/tables are deterministic zero; FLAGS reserved bit1 is canonical1.
 * Invalid menu returns0 without changing either object. The CPU-only reset of port F0h is menu_shut.h. */
int pc98_cpu_menu_reset(unsigned,Pc98Mode286 *,Pc98Cpu386State *);
#endif
