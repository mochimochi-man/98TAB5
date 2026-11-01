/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "interrupt286.h"
#include "cpuprof.h"
#include "purge.h"
#include <assert.h>
int pc98_cpu_deliver_gate286(Pc98Cpu16State *c,Pc98InterruptTables286 *t,
 const Pc98ExecuteBus *b,unsigned vector,int software,int external,int has_error,
 uint16_t original_error,Pc98CpuFault16 *failure) {
 assert(c->model==PC98_CPU_286&&vector<256);
 Pc98ProtectedSegments286 *p=t->segments;unsigned cpl=pc98_cpu_cpl16(c);
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Gate286 gate;uint16_t error=0;
 unsigned fault=pc98_cpu_lookup_gate286(&fetch,&t->idt,vector,external,&gate,&error);
 if(fault)goto rejected;
 fault=pc98_cpu_check_gate286(&gate,vector,cpl,software,external,&error);
 if(fault)goto rejected;
 if((gate.access&31u)==5){
  PROF_EVT(PEV_INT_TASKGATE);
#if PC98_FULLCPU
  if(!t->task_switch)return -1;
  return pc98_cpu_switch_task286(c,t->task_switch,b,gate.selector,PC98_TASK_INTERRUPT,0,external,has_error,original_error,failure);
#else
  PURGE_STOP("task gate (interrupt/exception)");return -1;
#endif
 }
 Pc98Descriptor286 code;
 fault=pc98_cpu_lookup286(&fetch,&p->tables,gate.selector,&code,&error);
 if(fault){error|=(uint16_t)(external?1:0);goto rejected;}
 Pc98Descriptor286 probe=code;probe.access|=128;probe.limit=0xffff;unsigned next_cpl=99;
 fault=pc98_cpu_interrupt_code286(&probe,gate.selector,cpl,0,&next_cpl,&error);
 if(fault){error|=(uint16_t)(external?1:0);goto rejected;}
 unsigned transition=next_cpl!=cpl;uint16_t top=c->r[PC98_SP],new_ss=c->seg[PC98_SS];Pc98Descriptor286 stack={0};
 if(transition){
  if(!t->task)return -1;
  fault=pc98_cpu_tss_stack286(t->task,&fetch,next_cpl,&top,&new_ss,&error);
  if(fault){error|=(uint16_t)(external?1:0);goto rejected;}
  fault=pc98_cpu_lookup286(&fetch,&p->tables,new_ss,&stack,&error);
  if(fault){fault=10;error=(uint16_t)((new_ss&0xfffcu)|(external?1u:0u));goto rejected;}
  error=(uint16_t)((new_ss&0xfffcu)|(external?1u:0u));
  if((new_ss&3u)!=next_cpl||((stack.access>>5)&3u)!=next_cpl||(stack.access&0x1au)!=0x12u){fault=10;goto rejected;}
  if(!(stack.access&128u)){fault=12;goto rejected;}
 }else if(!(code.access&128u)){fault=11;error=(uint16_t)((gate.selector&0xfffcu)|(external?1u:0u));goto rejected;}
 PROF_EVT(transition?PEV_INTGATE_INNER:PEV_INTGATE_SAME);PROF_EVT((gate.access&31u)==6?PEV_INTGATE_T6:PEV_INTGATE_T7);
 unsigned bytes=(transition?10u:6u)+(has_error?2u:0u);uint16_t lowest=(uint16_t)(top-bytes);uint32_t physical;
 if(transition){
  if(!pc98_cpu_range286(&stack,lowest,bytes)){fault=12;error=(uint16_t)((new_ss&0xfffcu)|(external?1u:0u));goto rejected;}
  physical=(stack.base+lowest)&0xffffffu;
 }else{
  fault=pc98_cpu_translate286(p,PC98_SS,lowest,bytes,PC98_SEG_WRITE,&physical);
  if(fault){error=(uint16_t)(external?1:0);goto rejected;}
 }
 if(!(code.access&128u)){fault=11;error=(uint16_t)((gate.selector&0xfffcu)|(external?1u:0u));goto rejected;}
 if(gate.offset>code.limit){fault=13;error=(uint16_t)(external?1:0);goto rejected;}
 /* All protection checks precede the first descriptor or stack write. */
 if(!(code.access&1)){
  const Pc98Table286 *table=(gate.selector&4u)?&p->tables.ldt:&p->tables.gdt;
  b->write8(b->context,(table->base+(gate.selector&0xfff8u)+5u)&0xffffffu,(uint8_t)(code.access|1));
 }
 if(transition&&!(stack.access&1)){
  const Pc98Table286 *table=(new_ss&4u)?&p->tables.ldt:&p->tables.gdt;
  b->write8(b->context,(table->base+(new_ss&0xfff8u)+5u)&0xffffffu,(uint8_t)(stack.access|1));
 }
 uint16_t values[6];unsigned count=0;
 if(transition){values[count++]=c->seg[PC98_SS];values[count++]=c->r[PC98_SP];}
 values[count++]=(uint16_t)((c->flags&0x7fd5u)|2u);values[count++]=c->seg[PC98_CS];values[count++]=c->ip;
 if(has_error)values[count++]=original_error;
 for(unsigned n=0;n<count;n++)b->write16(b->context,(physical+bytes-2*(n+1))&0xffffffu,values[n]);
 code.access|=1;p->cache[PC98_CS]=code;p->valid[PC98_CS]=1;
 if(transition){stack.access|=1;p->cache[PC98_SS]=stack;p->valid[PC98_SS]=1;c->seg[PC98_SS]=new_ss;c->base[PC98_SS]=stack.base;}
 pc98_cpu_set_cpl16(c,next_cpl);c->seg[PC98_CS]=(uint16_t)((gate.selector&0xfffcu)|next_cpl);c->base[PC98_CS]=code.base;c->ip=gate.offset;c->r[PC98_SP]=lowest;
 c->flags&=(uint16_t)~(0x4100u|((gate.access&31u)==6?0x200u:0u));
 c->irq_inhibit=c->nmi_inhibit=c->halted=c->debug_inhibit=0;c->fault=(Pc98CpuFault16){0};return 1;
rejected:
 *failure=(Pc98CpuFault16){.vector=(uint8_t)fault,.valid=1,.has_error=1,.error=error,.restart_ip=c->ip};return 0;
}
int pc98_cpu_interrupt_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,
 unsigned vector,int software,int external,int has_error,uint16_t error,Pc98CpuFault16 *failure) {
 return pc98_cpu_deliver_gate286(c,context,b,vector,software,external,has_error,error,failure);
}
static void invalidate_outer_data(Pc98Cpu16State *c,Pc98ProtectedSegments286 *p,unsigned cpl) {
 const unsigned segments[]={PC98_ES,PC98_DS};
 for(unsigned n=0;n<2;n++){
  unsigned segment=segments[n],selector=c->seg[segment],ar=p->cache[segment].access,dpl=(ar>>5)&3u;
  const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
  int valid=p->valid[segment]&&(selector&0xfffcu)&&(!(selector&4u)||p->tables.ldt_valid)&&((selector&0xfff8u)+7<=table->limit)&&(ar&16u)&&(!(ar&8u)||(ar&2u))&&(((ar&12u)==12u)||(dpl>=cpl&&dpl>=(selector&3u)));
  if(!valid){c->seg[segment]=0;c->base[segment]=0;p->valid[segment]=0;p->cache[segment]=(Pc98Descriptor286){0};}
 }
}
static int return_protected(Pc98Cpu16State *c,Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,int has_flags,uint16_t release,Pc98CpuFault16 *failure) {
 assert(c->model==PC98_CPU_286);
 if(has_flags&&(c->flags&0x4000u))return -1;
 uint16_t error=0;uint32_t physical;
 /* The return selector must be inspected before choosing the full frame size. */
 unsigned fault=pc98_cpu_translate286(p,PC98_SS,c->r[PC98_SP],4,PC98_SEG_READ,&physical);
 if(fault)goto rejected;
 uint16_t target=b->read16(b->context,physical),selector=b->read16(b->context,(physical+2)&0xffffffu);
 unsigned cpl=pc98_cpu_cpl16(c),rpl=selector&3u;
 if(rpl<cpl){fault=13;error=(uint16_t)(selector&0xfffcu);goto rejected;}
 unsigned outer=rpl>cpl,base_bytes=has_flags?6u:4u,bytes=base_bytes+(outer?4u+release:0u);
 fault=pc98_cpu_translate286(p,PC98_SS,c->r[PC98_SP],bytes,PC98_SEG_READ,&physical);if(fault)goto rejected;
 uint16_t flags=has_flags?b->read16(b->context,(physical+4)&0xffffffu):c->flags;
 uint16_t next_sp=(uint16_t)(c->r[PC98_SP]+base_bytes+release),next_ss=c->seg[PC98_SS];
 if(outer){unsigned slot=base_bytes+release;next_sp=(uint16_t)(b->read16(b->context,(physical+slot)&0xffffffu)+release);next_ss=b->read16(b->context,(physical+slot+2)&0xffffffu);}
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 code,stack={0};
 fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&code,&error);if(fault)goto rejected;
 /* B-52 requires conforming DPL>oldCPL; section11.2.1 requires DPL<=RPL.
  * Support their common permitted range only. Differing cases remain
  * pending; do not turn the manual ambiguity into a guessed#GP. */
 if(has_flags&&outer&&(code.access&28u)==28u){unsigned dpl=(code.access>>5)&3u;if(dpl<=cpl||dpl>rpl)return -1;}
 Pc98Descriptor286 probe=code;probe.limit=0xffff;
 fault=pc98_cpu_check_code286(&probe,selector,rpl,0,&error);if(fault)goto rejected;
 if(outer){
  fault=pc98_cpu_lookup286(&fetch,&p->tables,next_ss,&stack,&error);if(fault)goto rejected;
  fault=pc98_cpu_load_data286(&stack,next_ss,rpl,1,&error);if(fault)goto rejected;
 }
 if(target>code.limit){fault=13;error=0;goto rejected;}
 uint16_t new_flags=c->flags;
 if(has_flags){
  unsigned iopl=(c->flags>>12)&3u;new_flags=(uint16_t)((flags&0x4fd5u)|2u);
  new_flags|=(uint16_t)(cpl?c->flags&0x3000u:flags&0x3000u);
  if(cpl>iopl)new_flags=(uint16_t)((new_flags&~0x200u)|(c->flags&0x200u));
 }
 if(!(code.access&1)){
  const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
  b->write8(b->context,(table->base+(selector&0xfff8u)+5u)&0xffffffu,(uint8_t)(code.access|1));
 }
 if(outer&&!(stack.access&1)){
  const Pc98Table286 *table=(next_ss&4u)?&p->tables.ldt:&p->tables.gdt;
  b->write8(b->context,(table->base+(next_ss&0xfff8u)+5u)&0xffffffu,(uint8_t)(stack.access|1));
 }
 code.access|=1;p->cache[PC98_CS]=code;p->valid[PC98_CS]=1;
 if(outer){stack.access|=1;p->cache[PC98_SS]=stack;p->valid[PC98_SS]=1;c->seg[PC98_SS]=next_ss;c->base[PC98_SS]=stack.base;}
 pc98_cpu_set_cpl16(c,rpl);c->seg[PC98_CS]=selector;c->base[PC98_CS]=code.base;c->ip=target;c->flags=new_flags;c->r[PC98_SP]=next_sp;
 if(outer)invalidate_outer_data(c,p,rpl);
 if(has_flags)c->nmi_active=0;
 c->fault=(Pc98CpuFault16){0};return 1;
rejected:
 *failure=(Pc98CpuFault16){.vector=(uint8_t)fault,.valid=1,.has_error=1,.error=error,.restart_ip=c->ip};return 0;
}
int pc98_cpu_iret286(Pc98Cpu16State *c,Pc98InterruptTables286 *t,const Pc98ExecuteBus *b,Pc98CpuFault16 *failure) {
#if !PC98_FULLCPU
 if(c->flags&0x4000u){PURGE_STOP("IRET with NT (task return)");return -1;}
#endif
 if((c->flags&0x4000u)&&t->task_switch){
  Pc98TaskRegisters286 *tr=t->task_switch->task;
  if(!tr->tr_valid||tr->tss.limit<1)return -1;
  uint16_t selector=b->read16(b->context,tr->tss.base);
  return pc98_cpu_switch_task286(c,t->task_switch,b,selector,PC98_TASK_IRET,0,0,0,0,failure);
 }
 return return_protected(c,t->segments,b,1,0,failure);
}
int pc98_cpu_far_return286(Pc98Cpu16State *c,Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,uint16_t release,Pc98CpuFault16 *failure) {
 return return_protected(c,p,b,0,release,failure);
}
int pc98_cpu_far_return_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t release,Pc98CpuFault16 *failure) {
 return pc98_cpu_far_return286(c,context,b,release,failure);
}
int pc98_cpu_iret_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,Pc98CpuFault16 *failure) {
 return pc98_cpu_iret286(c,context,b,failure);
}
int pc98_cpu_deliver_pending286(Pc98Cpu16State *c,Pc98InterruptTables286 *t,const Pc98ExecuteBus *b,Pc98CpuFault16 *failure) {
 if(!c->fault.valid)return -1;
 Pc98Cpu16State saved=*c;Pc98CpuFault16 event=c->fault;c->ip=event.restart_ip;
 int result=pc98_cpu_deliver_gate286(c,t,b,event.vector,0,0,event.has_error,event.error,failure);
 if(result==0&&failure->committed)c->fault=*failure;
 else if(result!=1)*c=saved;
 return result;
}
