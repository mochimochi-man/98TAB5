/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_MENU_SHUT_H
#define PC98_CPU_MENU_SHUT_H
#include "menu_reset.h"
/* The CPU-only reset of port F0h: the processor's RESET (menu_reset state) with the upper halves of the general
 * registers kept (undefined after RESET, Intel386 PRM 10.1). The caller keeps clocks, events and devices, clears
 * the reset request and turns A20 off through the memory bus. An invalid menu changes nothing. */
int pc98_cpu_menu_shut(unsigned,Pc98Mode286 *,Pc98Cpu386State *);
#endif
