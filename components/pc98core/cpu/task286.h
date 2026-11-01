/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80286 PRM 8.2, LLDT/LTR and 286 TSS layout. */
#ifndef PC98_CPU_TASK286_H
#define PC98_CPU_TASK286_H
#include "protected286.h"
typedef struct {uint16_t backlink,sp[3],ss[3],ip,flags,r[8],seg[4],ldt;} Pc98Tss286;
Pc98Tss286 pc98_cpu_parse_tss286(const uint8_t bytes[44]);
/* Save only dynamic bytes14..41; preserve backlink, privilege stacks and
 * the static LDT selector (Intel286 PRM8.2).
 * Does not switch tasks, mark busy, or access memory. */
void pc98_cpu_save_tss286(uint8_t bytes[44],const Pc98Cpu16State *);
typedef struct {uint16_t tr,ldtr;Pc98Descriptor286 tss;uint8_t tr_valid;} Pc98TaskRegisters286;
/* Pure protected-mode instruction helpers; caller checks CPL0 and source
 * operand before invoking. LLDT leaves ordinary segment caches unchanged.
 * LTR marks available TSS busy but does not switch tasks or check switch-only
 * minimum TSS size. All protection failures leave memory/state unchanged. */
unsigned pc98_cpu_lldt286(Pc98TaskRegisters286 *,Pc98ProtectedSegments286 *,const Pc98ExecuteBus *,uint16_t selector,uint16_t *error);
unsigned pc98_cpu_ltr286(Pc98TaskRegisters286 *,const Pc98ProtectedSegments286 *,const Pc98ExecuteBus *,uint16_t selector,uint16_t *error);
/* Read one cached-TSS privilege stack pair, entire range checked first.
 * Returns 0 or #TS(TR selector), output pair unchanged on failure. */
unsigned pc98_cpu_tss_stack286(const Pc98TaskRegisters286 *,const Pc98FetchBus *,unsigned privilege,uint16_t *sp,uint16_t *ss,uint16_t *error);
#endif
