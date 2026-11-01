/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_BOUNDARY_H
#define PC98_CPU_BOUNDARY_H
#include "menu_cpu.h"
/* A snapshot of caller-owned external requests. No request is cleared here;
 * PIC acknowledgement and NMI edge latches remain outside the CPU. */
typedef struct {unsigned irq_vector;uint8_t irq,nmi;} Pc98BoundaryRequest;
enum Pc98BoundaryAction {PC98_BOUNDARY_STEP,PC98_BOUNDARY_EXTERNAL};
typedef struct {
 enum Pc98BoundaryAction action;
 enum Pc98StepResult step;
 enum Pc98InterruptResult interrupt;
 enum Pc98InterruptKind kind;
} Pc98BoundaryResult;
/* One externally visible boundary: NMI, then IRQ, then one backend step.
 * Blocked requests allow execution; unsupported/faulted entry stops here.
 * REP may return a partial iteration. No timing, device acknowledgement or
 * exception delivery is invented. completion output is zero for external
 * entry, and follows the backend's whole-instruction contract otherwise. */
Pc98BoundaryResult pc98_cpu_boundary(unsigned,Pc98Mode286 *,Pc98Cpu386State *,const Pc98ExecuteBus *,const Pc98BoundaryRequest *);
#endif
