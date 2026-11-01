/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_SYSTEM_EXECUTE286_H
#define PC98_CPU_SYSTEM_EXECUTE286_H
#include "system286.h"
#include "task286.h"
typedef struct {Pc98ProtectedSegments286 *segments;Pc98TaskRegisters286 *task;Pc98Table286 *idt;uint16_t msw;uint8_t npx_busy;int (*enable_protected)(void *,Pc98Cpu16State *);void *mode_context;int (*load_all)(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,Pc98CpuFault16 *);} Pc98SystemState286;
/* Atomic system hook: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/CLTS,
 * SLDT/STR/LLDT/LTR/VERR/VERW/LAR/LSL and ARPL. PE-enable transition returns
 * pending unless the optional mode hook installs protected caches/CPL.
 * A failing enable_protected hook must leave CPU and external state unchanged.
 * WAIT samples npx_busy; ESC implements the MSW #NM control trap only,
 * returning unsupported when actual coprocessor execution is required.
 * LOADALL requires its optional mode hook; task switching has a separate hook. */
int pc98_cpu_system_execute286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,
 unsigned opcode,const Pc98Operand16 *,Pc98CpuFault16 *);
#endif
