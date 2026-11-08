/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80286 PRM CALL B-24/B-25 and inter-level call frame. */
#include "call286.h"
#include "purge.h"
#include <assert.h>
static void accessed(Pc98Descriptor286 *d,const Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,uint16_t selector){
 if(d->access&1)return;
 const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
 d->access|=1;b->write8(b->context,(table->base+(selector&0xfff8u)+5u)&0xffffffu,d->access);
}
int pc98_cpu_call_gate286(Pc98Cpu16State *c,Pc98CallContext286 *context,const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,Pc98CpuFault16 *failure){
 assert(c->model==PC98_CPU_286);Pc98ProtectedSegments286 *p=context->segments;uint16_t error=0;
 unsigned fault=pc98_cpu_call_code286(c,p,b,selector,offset,&error);
 if(!fault)return 1;
 if(fault!=PC98_PROTECTED_SYSTEM_PENDING)goto rejected;
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 gate;
 fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&gate,&error);if(fault)goto rejected;
 unsigned type=gate.access&31u;
 #if PC98_FULLCPU
 if(type==1||type==3){if(!context->task_switch)return -1;return pc98_cpu_switch_task286(c,context->task_switch,b,selector,PC98_TASK_CALL,1,0,0,0,failure);}
 if(type==5&&!context->task_switch)return -1;
#else
 if(type==1||type==3){PURGE_STOP("TSS task switch (CALL to TSS)");return -1;}
 if(type==5){PURGE_STOP("task gate (CALL)");return -1;}
#endif
 error=(uint16_t)(selector&0xfffcu);unsigned cpl=pc98_cpu_cpl16(c),gate_dpl=(gate.access>>5)&3u;
 if((type!=4&&type!=5)||gate_dpl<cpl||gate_dpl<(selector&3u)){fault=13;goto rejected;}
 if(!(gate.access&128u)){fault=11;goto rejected;}
 
#if PC98_FULLCPU
 if(type==5)return pc98_cpu_switch_task286(c,context->task_switch,b,(uint16_t)gate.base,PC98_TASK_CALL,0,0,0,0,failure);
#endif
 /* A gate's raw bytes2..4 occupy base in the descriptor decoder. */
 uint16_t code_selector=(uint16_t)gate.base,target=gate.limit;unsigned count=(gate.base>>16)&31u;
 Pc98Descriptor286 code,stack={0};
 fault=pc98_cpu_lookup286(&fetch,&p->tables,code_selector,&code,&error);if(fault)goto rejected;
 Pc98Descriptor286 probe=code;probe.limit=0xffff;unsigned next_cpl=99;
 fault=pc98_cpu_interrupt_code286(&probe,code_selector,cpl,0,&next_cpl,&error);if(fault)goto rejected;
 unsigned transition=next_cpl!=cpl;uint16_t top=c->r[PC98_SP],new_ss=c->seg[PC98_SS];uint32_t source=0,physical;
 
#if !PC98_FULLCPU
 if(transition){PURGE_STOP("call gate privilege transition");return -1;}
#endif
 if(transition){
  if(!context->task)return -1;
  fault=pc98_cpu_tss_stack286(context->task,&fetch,next_cpl,&top,&new_ss,&error);if(fault)goto rejected;
  fault=pc98_cpu_lookup286(&fetch,&p->tables,new_ss,&stack,&error);
  if(fault){fault=10;error=(uint16_t)(new_ss&0xfffcu);goto rejected;}
  error=(uint16_t)(new_ss&0xfffcu);
  if((new_ss&3u)!=next_cpl||((stack.access>>5)&3u)!=next_cpl||(stack.access&0x1au)!=0x12u){fault=10;goto rejected;}
  if(!(stack.access&128u)){fault=12;goto rejected;}
 }
 unsigned bytes=transition?8+count*2:4;uint16_t lowest=(uint16_t)(top-bytes);
 if(transition){
  if(!pc98_cpu_range286(&stack,lowest,bytes)){fault=12;error=0;goto rejected;}
  physical=(stack.base+lowest)&0xffffffu;
  if(count){fault=pc98_cpu_translate286(p,PC98_SS,c->r[PC98_SP],count*2,PC98_SEG_READ,&source);if(fault){error=0;goto rejected;}}
 }else{fault=pc98_cpu_translate286(p,PC98_SS,lowest,4,PC98_SEG_WRITE,&physical);if(fault){error=0;goto rejected;}}
 if(target>code.limit){fault=13;error=0;goto rejected;}
 accessed(&code,p,b,code_selector);if(transition)accessed(&stack,p,b,new_ss);
 unsigned slot=bytes;
 if(transition){
  slot-=2;b->write16(b->context,(physical+slot)&0xffffffu,c->seg[PC98_SS]);slot-=2;b->write16(b->context,(physical+slot)&0xffffffu,c->r[PC98_SP]);
  for(unsigned n=count;n>0;n--){uint16_t value=b->read16(b->context,(source+2*(n-1))&0xffffffu);slot-=2;b->write16(b->context,(physical+slot)&0xffffffu,value);}
 }
 slot-=2;b->write16(b->context,(physical+slot)&0xffffffu,c->seg[PC98_CS]);slot-=2;b->write16(b->context,(physical+slot)&0xffffffu,c->ip);
 if(transition){p->cache[PC98_SS]=stack;p->valid[PC98_SS]=1;c->seg[PC98_SS]=new_ss;c->base[PC98_SS]=stack.base;}
 p->cache[PC98_CS]=code;p->valid[PC98_CS]=1;pc98_cpu_set_cpl16(c,next_cpl);c->seg[PC98_CS]=(uint16_t)((code_selector&0xfffcu)|next_cpl);c->base[PC98_CS]=code.base;c->ip=target;c->r[PC98_SP]=lowest;return 1;
rejected:
 *failure=(Pc98CpuFault16){.valid=1,.has_error=1,.vector=(uint8_t)fault,.error=error,.restart_ip=c->ip};return 0;
}
int pc98_cpu_call_gate_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,Pc98CpuFault16 *failure){
 return pc98_cpu_call_gate286(c,context,b,selector,offset,failure);
}
