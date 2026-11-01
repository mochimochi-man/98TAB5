/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_TASK_SWITCH286_H
#define PC98_CPU_TASK_SWITCH286_H
#include "task286.h"
enum Pc98TaskTransfer286 {PC98_TASK_CALL,PC98_TASK_JUMP,PC98_TASK_IRET,PC98_TASK_INTERRUPT};
typedef struct Pc98TaskSwitch286 {Pc98ProtectedSegments286 *segments;Pc98TaskRegisters286 *task;uint16_t *msw;} Pc98TaskSwitch286;
/* 1 switched, 0 fault, -1 unsupported malformed old/new cache boundary.
 * Precommit faults preserve all state. After the task switch commits,
 * failure.committed=1 and CPU/TR/cache/TSS writes remain in incoming context.
 * Caller MUST NOT restore outgoing CPU on a committed fault.
 * Short incoming TSS (<43), invalid/short outgoing cache (<41), self selector
 * and missing MSW context stay pending until their partial-switch semantics
 * are independently established. check_privilege applies direct CALL/JMP
 * TSS DPL checks; task gates already checked their own DPL.
 * Caller provides the correct outgoing next-IP and handles LOCK/scheduling. */
int pc98_cpu_switch_task286(Pc98Cpu16State *,Pc98TaskSwitch286 *,const Pc98ExecuteBus *,
 uint16_t selector,enum Pc98TaskTransfer286,int check_privilege,int external,
 int has_error,uint16_t error,Pc98CpuFault16 *failure);
/* Full far JMP endpoint: direct code, same-level call gate, TSS/task gate. */
int pc98_cpu_jump_task_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,uint16_t selector,uint16_t offset,Pc98CpuFault16 *);

#endif
