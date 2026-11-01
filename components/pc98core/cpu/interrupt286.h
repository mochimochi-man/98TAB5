/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_INTERRUPT286_H
#define PC98_CPU_INTERRUPT286_H
#include "gate286.h"
#include "protected286.h"
#include "task286.h"
#include "task_switch286.h"
typedef struct {Pc98ProtectedSegments286 *segments;Pc98Table286 idt;Pc98TaskRegisters286 *task;Pc98TaskSwitch286 *task_switch;} Pc98InterruptTables286;
/* Atomic interrupt/trap gate delivery (optional TSS privilege stacks), Intel 286 PRM 9.4/Table9-1.
 * 1 delivered, 0 protection fault, -1 pending task/privilege transition.
 * Caller handles event priority, IRQ/NMI inhibition and double faults.
 * Failure/pending leave CPU, caches and memory unchanged. Bus writes cannot
 * fail. has_error/error describe the original event's optional frame word. */
int pc98_cpu_deliver_gate286(Pc98Cpu16State *,Pc98InterruptTables286 *,
 const Pc98ExecuteBus *,unsigned vector,int software,int external,int has_error,
 uint16_t error,Pc98CpuFault16 *failure);
int pc98_cpu_interrupt_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,
 unsigned vector,int software,int external,int has_error,uint16_t error,Pc98CpuFault16 *);
/* Same-CPL and outer non-conforming IRET. Outer conforming IRET supports
 * only the common range of PRM11.2.1/B-52: DPL>oldCPL and DPL<=returnRPL.
 * Disputed conforming cases remain pending. NT uses the optional task adapter. */
int pc98_cpu_iret286(Pc98Cpu16State *,Pc98InterruptTables286 *,const Pc98ExecuteBus *,Pc98CpuFault16 *);
int pc98_cpu_iret_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,Pc98CpuFault16 *);
/* Explicit pending-fault dispatch. Secondary-fault/double-fault policy remains
 * the caller's responsibility; failed delivery preserves the original fault. */
int pc98_cpu_deliver_pending286(Pc98Cpu16State *,Pc98InterruptTables286 *,const Pc98ExecuteBus *,Pc98CpuFault16 *);

/* Far RET: same/outer privilege return, parameters released on both stacks.
 * Preserves FLAGS and NMI-active; the new outer SP is checked on later use. */
int pc98_cpu_far_return286(Pc98Cpu16State *,Pc98ProtectedSegments286 *,const Pc98ExecuteBus *,uint16_t release,Pc98CpuFault16 *);
int pc98_cpu_far_return_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,uint16_t release,Pc98CpuFault16 *);

#endif
