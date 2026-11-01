/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80286 PRM 10.4.2: LMSW enables PE without reloading selectors.
 * Descriptor tables are consulted on subsequent explicit segment loads. */
#include "mode286.h"
#include "cpuprof.h"
#include "real286.h"
#include "loadall286.h"
#include <string.h>
static void initialize_cache(Pc98Mode286 *m,const Pc98Cpu16State *c){
 if(m->cache_initialized)return;
 for(unsigned s=0;s<4;s++){m->segments.cache[s]=(Pc98Descriptor286){.base=c->base[s],.limit=0xffff,.access=(uint8_t)(s==PC98_CS?0x9b:0x93)};m->segments.valid[s]=1;}
 m->cache_initialized=1;
}
static int enable(void *context,Pc98Cpu16State *c){
 Pc98Mode286 *m=context;if(c->model!=PC98_CPU_286)return 0;initialize_cache(m,c);PROF_EVT(PEV_PE_ENTER);
 c->cpl=(uint8_t)((m->segments.cache[PC98_CS].access>>5)&3u);c->cpl_valid=1;return 1;
}
static int load_all(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,Pc98CpuFault16 *failure){
 (void)failure;Pc98Mode286 *m=context;uint8_t bytes[102];PROF_EVT(PEV_LOADALL);
 for(unsigned i=0;i<102;i+=2){uint16_t value=b->read16(b->context,0x800u+i);bytes[i]=(uint8_t)value;bytes[i+1]=(uint8_t)(value>>8);}
 Pc98Loadall286 s=pc98_cpu_parse_loadall286(bytes);s.msw|=m->system.msw&1u;
 m->system.msw=s.msw;m->task=(Pc98TaskRegisters286){.tr=s.tr,.ldtr=s.ldtr,.tss=s.tss,.tr_valid=(uint8_t)((s.tss.access&128u)!=0)};
 m->segments.tables=(Pc98DescriptorTables286){.gdt=s.gdt,.ldt={s.ldt.base,s.ldt.limit},.ldt_valid=(uint8_t)((s.ldt.access&128u)!=0)};m->interrupts.idt=s.idt;
 memcpy(c->r,s.r,sizeof(c->r));memcpy(c->seg,s.seg,sizeof(c->seg));memcpy(m->segments.cache,s.cache,sizeof(s.cache));
 for(unsigned segment=0;segment<4;segment++){c->base[segment]=s.cache[segment].base;m->segments.valid[segment]=(uint8_t)((s.cache[segment].access&128u)!=0);}
 c->ip=s.ip;c->flags=(uint16_t)((s.flags&0x7fd5u)|2u);c->cpl=(uint8_t)((s.msw&1u)?(s.cache[PC98_CS].access>>5)&3u:0);c->cpl_valid=1;m->cache_initialized=1;return 1;
}
void pc98_cpu_mode_init286(Pc98Mode286 *m){
 memset(m,0,sizeof(*m));
 m->system=(Pc98SystemState286){.segments=&m->segments,.task=&m->task,.idt=&m->interrupts.idt,.msw=0xfff0,.enable_protected=enable,.mode_context=m,.load_all=load_all};
 m->task_switch=(Pc98TaskSwitch286){.segments=&m->segments,.task=&m->task,.msw=&m->system.msw};
 m->calls=(Pc98CallContext286){.segments=&m->segments,.task=&m->task,.task_switch=&m->task_switch};
 m->interrupts=(Pc98InterruptTables286){.segments=&m->segments,.task=&m->task,.task_switch=&m->task_switch,.idt={0,0x3ff}};
}
Pc98ExecuteBus pc98_cpu_mode_bus286(Pc98Mode286 *m,Pc98Cpu16State *c,const Pc98ExecuteBus *io){
 Pc98ExecuteBus b=*io;b.system_instruction=pc98_cpu_system_execute286;b.system_context=&m->system;b.real_mode=(uint8_t)(!(m->system.msw&1u));b.single_step=1;b.task_committed=0;
 if(m->system.msw&1u){
  b.resolve=pc98_cpu_resolve_segment286;b.resolve_context=&m->segments;
  b.load_segment=pc98_cpu_load_segment_bridge286;b.segment_context=&m->segments;
  b.jump_far=pc98_cpu_jump_task_bridge286;b.jump_context=&m->task_switch;
  b.call_far=pc98_cpu_call_gate_bridge286;b.call_context=&m->calls;b.return_far=pc98_cpu_far_return_bridge286;
  b.interrupt_gate=pc98_cpu_interrupt_bridge286;b.return_gate=pc98_cpu_iret_bridge286;b.interrupt_context=&m->interrupts;
 }else{
  initialize_cache(m,c);b.resolve=pc98_cpu_real_resolve286;b.resolve_context=&m->segments;
  b.load_segment=pc98_cpu_real_load286;b.segment_context=m;b.jump_far=pc98_cpu_real_jump286;b.jump_context=m;
  b.call_far=pc98_cpu_real_call286;b.call_context=m;b.return_far=pc98_cpu_real_return286;
  b.interrupt_gate=pc98_cpu_real_interrupt286;b.return_gate=pc98_cpu_real_iret286;b.interrupt_context=m;
 }
 return b;
}
enum Pc98StepResult pc98_cpu_mode_step286(Pc98Mode286 *m,Pc98Cpu16State *c,const Pc98ExecuteBus *io){
 if(c->model!=PC98_CPU_286)return PC98_STEP_UNSUPPORTED;
 if(m->shutdown)return PC98_STEP_SHUTDOWN;
 Pc98ExecuteBus b=pc98_cpu_mode_bus286(m,c,io);unsigned switched=0;b.task_committed=&switched;return pc98_cpu_step16(c,&b);
}
enum Pc98InterruptResult pc98_cpu_mode_interrupt286(Pc98Mode286 *m,Pc98Cpu16State *c,const Pc98ExecuteBus *io,unsigned vector,enum Pc98InterruptKind kind){
 if(c->model!=PC98_CPU_286)return PC98_INT_UNSUPPORTED;
 if(m->shutdown){
  if(vector>255u||kind<PC98_INT_SOFTWARE||kind>PC98_INT_NMI||(kind==PC98_INT_NMI&&vector!=2u))return PC98_INT_UNSUPPORTED;
  if(kind!=PC98_INT_NMI||m->shutdown==2)return PC98_INT_BLOCKED;
  Pc98Cpu16State saved=*c;c->fault=(Pc98CpuFault16){0};c->nmi_inhibit=c->nmi_active=0;
  Pc98ExecuteBus b=pc98_cpu_mode_bus286(m,c,io);enum Pc98InterruptResult result=pc98_cpu_interrupt16(c,&b,vector,kind);
  if(result==PC98_INT_DELIVERED)m->shutdown=0;
  else if(result==PC98_INT_FAULT){m->delivery_fault=c->fault;if(!c->fault.committed)*c=saved;m->shutdown=2;}
  else *c=saved;
  return result;
 }
 Pc98ExecuteBus b=pc98_cpu_mode_bus286(m,c,io);return pc98_cpu_interrupt16(c,&b,vector,kind);
}

/* Intel 286 PRM 9.6.2: unlike later generation classification, the first
 * fault being 0/10/11/12/13 is sufficient. Handler-instruction faults are new
 * events; only transfer into a handler participates in this loop. */
static int contributory(unsigned vector){return vector==0||(vector>=10&&vector<=13);}
enum Pc98FaultDispatch286 pc98_cpu_mode_deliver_pending286(Pc98Mode286 *m,Pc98Cpu16State *c,const Pc98ExecuteBus *io){
 if(m->shutdown)return PC98_FAULT_SHUTDOWN;
 if(!c->fault.valid)return PC98_FAULT_NONE;
 if(c->model!=PC98_CPU_286)return PC98_FAULT_UNSUPPORTED;
 for(unsigned attempt=0;attempt<4;attempt++){
  Pc98Cpu16State saved=*c;Pc98CpuFault16 event=c->fault,failure={0};c->ip=event.restart_ip;c->fault=(Pc98CpuFault16){0};
  Pc98ExecuteBus b=pc98_cpu_mode_bus286(m,c,io);
  int result=b.interrupt_gate(b.interrupt_context,c,&b,event.vector,0,event.external,event.has_error,event.error,&failure);
  if(result==1){c->fault=(Pc98CpuFault16){0};return PC98_FAULT_DELIVERED;}
  if(result<0){*c=saved;return PC98_FAULT_UNSUPPORTED;}
  uint16_t restart=failure.committed?c->ip:event.restart_ip;
  if(!failure.committed)*c=saved;
  failure.valid=1;failure.external=event.external;failure.restart_ip=restart;m->delivery_fault=failure;
  if(event.vector==8){c->fault=failure;m->shutdown=1;return PC98_FAULT_SHUTDOWN;}
  if(contributory(event.vector))c->fault=(Pc98CpuFault16){.valid=1,.vector=8,.has_error=(uint8_t)((m->system.msw&1u)!=0),.restart_ip=restart,.committed=failure.committed,.external=event.external};
  else c->fault=failure;
 }
 return PC98_FAULT_UNSUPPORTED;
}
