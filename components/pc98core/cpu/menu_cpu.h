/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_MENU_CPU_H
#define PC98_MENU_CPU_H
#include "mode386.h"
/* Persisted menu:0 V30,1 286,2 286+extensions. Host-only initial dispatch.
 * Mode2 automatic dispatch currently supports real-mode286-compatible native
 * instructions and a protected subset without CPL/task transitions, plus
 * explicit extensions borrowing286 caches. V86 and absent
 * extension forms fail explicitly. No speculative execution/fallback.
 * LOADALL is the286 compatibility operation, not physical386 behavior.
 * Caller initializes mode in place and owns CPU/reset/IRQ/timing state. */
enum Pc98StepResult pc98_cpu_menu_step(unsigned,Pc98Mode286 *,Pc98Cpu386State *,const Pc98ExecuteBus *);
/* External IRQ/NMI only, at a caller-selected instruction boundary. Blocked
 * requests are retained by the caller. Does not retire an instruction or
 * alter instruction_completed. Mode2 PE/V86/task/low-SP entry is pending. */
enum Pc98InterruptResult pc98_cpu_menu_interrupt(unsigned,Pc98Mode286 *,Pc98Cpu386State *,const Pc98ExecuteBus *,unsigned,enum Pc98InterruptKind);
#endif
