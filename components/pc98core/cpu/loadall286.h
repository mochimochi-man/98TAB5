/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_LOADALL286_H
#define PC98_CPU_LOADALL286_H
#include "descriptor286.h"
/* Intel, Undocumented iAPX 286 Test Instruction, pp.2-3. Physical 0800..0865. */
typedef struct {
 uint16_t msw,tr,flags,ip,ldtr,seg[4],r[8];
 Pc98Descriptor286 cache[4],ldt,tss;
 Pc98Table286 gdt,idt;
} Pc98Loadall286;
Pc98Loadall286 pc98_cpu_parse_loadall286(const uint8_t image[102]);
#endif
