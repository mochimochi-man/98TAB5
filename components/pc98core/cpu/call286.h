/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_CALL286_H
#define PC98_CPU_CALL286_H
#include "gate286.h"
#include "task286.h"
#include "task_switch286.h"
typedef struct {Pc98ProtectedSegments286 *segments;Pc98TaskRegisters286 *task;Pc98TaskSwitch286 *task_switch;} Pc98CallContext286;
/* Direct code or call-gate CALL. Atomic protection checks precede descriptor
 * and stack writes. TSS/task gates stay pending. Parameters copy in defined
 * push/read order, including physically overlapping old/new stacks. */
int pc98_cpu_call_gate286(Pc98Cpu16State *,Pc98CallContext286 *,const Pc98ExecuteBus *,uint16_t selector,uint16_t offset,Pc98CpuFault16 *);
int pc98_cpu_call_gate_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,uint16_t selector,uint16_t offset,Pc98CpuFault16 *);
#endif
