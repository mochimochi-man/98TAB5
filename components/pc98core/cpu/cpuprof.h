/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Stage 13-0 measurement build (PC98_PROF=1 only). Not part of the normal build. */
#ifndef PC98_CPUPROF_H
#define PC98_CPUPROF_H
#ifdef PC98_PROF
enum {
    PEV_TASK_CALL, PEV_TASK_JUMP, PEV_TASK_IRET, PEV_TASK_INT,
    PEV_CALLGATE_SAME, PEV_CALLGATE_INNER, PEV_CALL_TASKGATE, PEV_CALL_TSS,
    PEV_INTGATE_SAME, PEV_INTGATE_INNER, PEV_INT_TASKGATE, PEV_INTGATE_T6, PEV_INTGATE_T7,
    PEV_LOADALL, PEV_PE_ENTER, PEV_PE_EXIT, PEV_MAX
};
extern unsigned long long g_prof_evt[PEV_MAX];
#define PROF_EVT(i) (g_prof_evt[(i)]++)
#else
#define PROF_EVT(i) ((void)0)
#endif
#endif
