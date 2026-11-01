/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_REAL286_H
#define PC98_CPU_REAL286_H
#include "execute.h"
int pc98_cpu_real_resolve286(void *,unsigned,uint32_t,uint32_t,enum Pc98MemoryAccess16,uint32_t *,Pc98CpuFault16 *);
int pc98_cpu_real_load286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,unsigned,uint16_t,Pc98CpuFault16 *);
int pc98_cpu_real_jump286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,uint16_t,uint16_t,Pc98CpuFault16 *);
int pc98_cpu_real_call286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,uint16_t,uint16_t,Pc98CpuFault16 *);
int pc98_cpu_real_return286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,uint16_t,Pc98CpuFault16 *);
int pc98_cpu_real_interrupt286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,unsigned,int,int,int,uint16_t,Pc98CpuFault16 *);
int pc98_cpu_real_iret286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,Pc98CpuFault16 *);
#endif
