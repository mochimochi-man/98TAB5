/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80286 PRM 9.3/9.4: IDT gate format and protection checks.
 * Pure foundation; not interrupt delivery or a task/privilege switch. */
#ifndef PC98_CPU_GATE286_H
#define PC98_CPU_GATE286_H
#include "descriptor286.h"
typedef struct {uint16_t offset,selector;uint8_t access;} Pc98Gate286;
Pc98Gate286 pc98_cpu_gate286(const uint8_t bytes[8]);
/* Failure leaves output unchanged, performs no read if outside IDT limit.
 * Error code is vector*8 + IDT + EXT. Physical reads wrap at 24 bits. */
unsigned pc98_cpu_lookup_gate286(const Pc98FetchBus *,const Pc98Table286 *,
 unsigned vector,int external,Pc98Gate286 *,uint16_t *error);
/* Task/interrupt/trap gates only. Software INT obeys gate DPL. Type and
 * privilege failures precede presence. Success does not execute the gate. */
unsigned pc98_cpu_check_gate286(const Pc98Gate286 *,unsigned vector,unsigned cpl,
 int software,int external,uint16_t *error);
/* Interrupt target code check; selector RPL ignored. Non-conforming target
 * may lower CPL, conforming retains it. new_cpl unchanged on failure.
 * Does not load caches, TSS stacks, or accessed bits. */
unsigned pc98_cpu_interrupt_code286(const Pc98Descriptor286 *,uint16_t selector,
 unsigned cpl,uint16_t offset,unsigned *new_cpl,uint16_t *error);
#endif
