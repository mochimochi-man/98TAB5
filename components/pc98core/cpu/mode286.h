/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_MODE286_H
#define PC98_CPU_MODE286_H
#include "system_execute286.h"
#include "call286.h"
#include "interrupt286.h"
/* Host-only adapter. Memory ownership/A20 remain in IO callbacks. Initialize
 * in place; do not copy this structure with its pointers to own members.
 * Cache/access faults return pending CPU faults in both modes; complete
 * exception priority, TF traps and timing remain incomplete. */
typedef struct {
 uint8_t cache_initialized;
 /* 1 allows one NMI recovery; 2 requires reset after failed NMI entry. */
 uint8_t shutdown;Pc98CpuFault16 delivery_fault;
 Pc98ProtectedSegments286 segments;
 Pc98TaskRegisters286 task;
 Pc98SystemState286 system;
 Pc98TaskSwitch286 task_switch;
 Pc98CallContext286 calls;
 Pc98InterruptTables286 interrupts;
} Pc98Mode286;
void pc98_cpu_mode_init286(Pc98Mode286 *);
/* Prepare cache/system/segment hooks for an explicitly selected backend.
 * Initializes real hidden caches in place; does not fetch or execute. Caller
 * checks model/shutdown first. Returned contexts borrow this mode object. */
Pc98ExecuteBus pc98_cpu_mode_bus286(Pc98Mode286 *,Pc98Cpu16State *,const Pc98ExecuteBus *);
enum Pc98StepResult pc98_cpu_mode_step286(Pc98Mode286 *,Pc98Cpu16State *,const Pc98ExecuteBus *);
enum Pc98InterruptResult pc98_cpu_mode_interrupt286(Pc98Mode286 *,Pc98Cpu16State *,const Pc98ExecuteBus *,unsigned vector,enum Pc98InterruptKind);
enum Pc98FaultDispatch286 {PC98_FAULT_NONE,PC98_FAULT_DELIVERED,PC98_FAULT_UNSUPPORTED,PC98_FAULT_SHUTDOWN};
/* Explicit fault delivery, including 286 contributory pairs and shutdown.
 * This does not fetch a handler instruction or synthesize timing. */
enum Pc98FaultDispatch286 pc98_cpu_mode_deliver_pending286(Pc98Mode286 *,Pc98Cpu16State *,const Pc98ExecuteBus *);
#endif
