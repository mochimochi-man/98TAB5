/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "mode386.h"
/* Reuse the already validated286 data-selector rules, but never borrow
 * the live DS slot as scratch. This value-only cache copy has no self-owned
 * pointers. Descriptor A update occurs only after successful validation. */
static int load_extra286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t selector,Pc98ExtraSegmentCache386 *out,Pc98CpuFault16 *failure){
 Pc98Mode286 *m=context;if(!(m->system.msw&1u))return -1;
 Pc98Cpu16State probe=*c;Pc98ProtectedSegments286 segments=m->segments;uint16_t error=0;
 unsigned vector=pc98_cpu_load_segment286(&probe,&segments,b,PC98_DS,selector,&error);
 if(vector){*failure=(Pc98CpuFault16){.valid=1,.vector=(uint8_t)vector,.has_error=1,.error=error};return 0;}
 const Pc98Descriptor286 *d=&segments.cache[PC98_DS];
 *out=(Pc98ExtraSegmentCache386){.base=d->base,.selector=selector,.limit=d->limit,.access=d->access,.initialized=1,.valid=segments.valid[PC98_DS]};return 1;
}
/* MOV r32,CR0 / MOV CR0,r32 on the 286 mode adapter. CR0 low bits are the 286 MSW (PE MP EM TS; ET reads 1,
 * as a 386 with a coprocessor), PE is sticky exactly like LMSW (set enables protected mode through the
 * adapter, clearing it is unsupported). PG (bit 31) set and CR1..CR4 are unsupported; CPL>0 raises #GP(0). */
static int control_register386(void *context,Pc98Cpu16State *c,unsigned write,unsigned number,uint32_t *value,Pc98CpuFault16 *failure){
 Pc98Mode286 *m=context;
 if(number!=0)return -1;
 unsigned protected_mode=m->system.msw&1u;
 if(protected_mode&&pc98_cpu_cpl16(c)){*failure=(Pc98CpuFault16){.valid=1,.vector=13,.has_error=1};return 0;}
 if(!write){*value=(uint32_t)(m->system.msw&15u)|0x10u;return 1;}
 if(*value&0x80000000u)return -1;
 uint16_t next=pc98_cpu_lmsw286(m->system.msw,(uint16_t)*value);
 if(protected_mode&&!(*value&1u))return -1;
 if(!protected_mode&&(next&1u)){
  if(!m->system.enable_protected||!m->system.enable_protected(m->system.mode_context,c))return -1;
 }
 m->system.msw=next;return 1;
}
Pc98ExecuteBus pc98_cpu_mode_bus_extension386(Pc98Mode286 *m,Pc98Cpu16State *c,const Pc98ExecuteBus *io){
 Pc98ExecuteBus b=pc98_cpu_mode_bus286(m,c,io);
 b.control_register386=control_register386;b.control_context=m;
 if(m->system.msw&1u){b.load_extra_segment=load_extra286;b.extra_segment_context=m;}
 return b;
}

enum Pc98StepResult pc98_cpu_mode_step_extension386(Pc98Mode286 *m,Pc98Cpu386State *s,const Pc98ExecuteBus *io){
 if(s->low.model!=PC98_CPU_286)return PC98_STEP_UNSUPPORTED;
 if(m->shutdown)return PC98_STEP_SHUTDOWN;
 Pc98ExecuteBus b=pc98_cpu_mode_bus_extension386(m,&s->low,io);
 return pc98_cpu_extension386(s,&b);
}
