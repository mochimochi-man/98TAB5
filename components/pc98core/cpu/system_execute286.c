/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "system_execute286.h"
#include "alu.h"
#include <assert.h>
static int rejected(const Pc98SystemState286 *s,Pc98CpuFault16 *f,unsigned vector,uint16_t error) {
 *f=(Pc98CpuFault16){.valid=1,.vector=(uint8_t)vector,.has_error=(uint8_t)((s->msw&1u)&&(vector==8||(vector>=10&&vector<=13))),.error=error,.external=(uint8_t)(vector==7||vector==9||vector==16)};return 0;
}
static int address(const Pc98SystemState286 *s,const Pc98Cpu16State *c,const Pc98ExecuteBus *b,const Pc98Operand16 *o,unsigned size,enum Pc98MemoryAccess16 access,uint32_t *physical,Pc98CpuFault16 *fault) {
 if(b->resolve)return b->resolve(b->resolve_context,o->segment,o->offset,size,access,physical,fault);
 if((uint32_t)o->offset+size>0x10000u)return rejected(s,fault,13,0);
 *physical=c->base[o->segment]+o->offset;return 1;
}
static int read_word(const Pc98SystemState286 *s,const Pc98Cpu16State *c,const Pc98ExecuteBus *b,const Pc98Operand16 *o,uint16_t *value,Pc98CpuFault16 *fault) {
 if(o->is_register){*value=c->r[o->rm];return 1;}
 uint32_t physical;if(!address(s,c,b,o,2,PC98_ACCESS_READ,&physical,fault))return 0;
 *value=b->read16(b->context,physical);return 1;
}
static int write_word(const Pc98SystemState286 *s,Pc98Cpu16State *c,const Pc98ExecuteBus *b,const Pc98Operand16 *o,uint16_t value,Pc98CpuFault16 *fault) {
 if(o->is_register){c->r[o->rm]=value;return 1;}
 uint32_t physical;if(!address(s,c,b,o,2,PC98_ACCESS_WRITE,&physical,fault))return 0;
 b->write16(b->context,physical,value);return 1;
}
int pc98_cpu_system_execute286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,
 unsigned opcode,const Pc98Operand16 *o,Pc98CpuFault16 *failure) {
 Pc98SystemState286 *s=context;assert(c->model==PC98_CPU_286);
 unsigned protected_mode=s->msw&1u,cpl=protected_mode?pc98_cpu_cpl16(c):0;
 /* Intel 286 PRM 10.1, Table 10-2: WAIT tests MP && TS; ESC
  * tests EM || TS. No operand access precedes the control trap. */
 if(opcode==0x9b){if((s->msw&10u)==10u)return rejected(s,failure,7,0);return s->npx_busy?2:1;}
 if(opcode>=0xd8&&opcode<=0xdf){if(s->msw&12u)return rejected(s,failure,7,0);return b->npx_absent?1:-1;}
 if((opcode==0x63||opcode==0xf00||opcode==0xf02||opcode==0xf03)&&!protected_mode)return rejected(s,failure,6,0);
 if(opcode==0xf05){if(cpl)return rejected(s,failure,13,0);return s->load_all?s->load_all(s->mode_context,c,b,failure):-1;}
 if(opcode==0xf06){if(cpl)return rejected(s,failure,13,0);s->msw&=(uint16_t)~8u;return 1;}
 assert(o);
 if(opcode==0x63){
  uint16_t value;uint32_t physical=0;
  if(!o->is_register&&!address(s,c,b,o,2,PC98_ACCESS_WRITE,&physical,failure))return 0;
  if(!read_word(s,c,b,o,&value,failure))return 0;
  uint16_t adjusted=pc98_cpu_arpl286(value,c->r[o->reg],&c->flags);
  if(adjusted!=value){if(o->is_register)c->r[o->rm]=adjusted;else b->write16(b->context,physical,adjusted);}
  return 1;
 }
 if(opcode==0xf02||opcode==0xf03){
  uint16_t selector;if(!read_word(s,c,b,o,&selector,failure))return 0;
  Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 descriptor;uint16_t error=0,value=0;
  unsigned vector=pc98_cpu_lookup286(&fetch,&s->segments->tables,selector,&descriptor,&error);
  int valid=!vector&&pc98_cpu_query_descriptor286(&descriptor,selector,cpl,opcode==0xf03,&value);
  if(valid)c->r[o->reg]=value;
  c->flags=(uint16_t)((c->flags&~PC98_ZF)|(valid?PC98_ZF:0));return 1;
 }
 if(opcode==0xf01){
  unsigned op=o->reg;
  if(op==5||op==7)return rejected(s,failure,6,0);
  if(op<=3&&o->is_register)return rejected(s,failure,6,0);
  if((op==2||op==3||op==6)&&cpl)return rejected(s,failure,13,0);
  if(op<=3){
   uint32_t physical;unsigned store=op<2;
   if(!address(s,c,b,o,6,store?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&physical,failure))return 0;
   Pc98Table286 *table=(op&1)?s->idt:&s->segments->tables.gdt;uint8_t bytes[6];
   if(store){pc98_cpu_table_store286(table,bytes);for(unsigned i=0;i<6;i++)b->write8(b->context,b->resolve?(physical+i)&0xffffffu:physical+i,bytes[i]);}
   else {for(unsigned i=0;i<6;i++)bytes[i]=b->read8(b->context,b->resolve?(physical+i)&0xffffffu:physical+i);*table=pc98_cpu_table_load286(bytes);}
   return 1;
  }
  if(op==4)return write_word(s,c,b,o,s->msw,failure);
  if(op==6){uint16_t value;if(!read_word(s,c,b,o,&value,failure))return 0;uint16_t next=pc98_cpu_lmsw286(s->msw,value);if(!protected_mode&&(next&1u)){if(!s->enable_protected)return -1;if(!s->enable_protected(s->mode_context,c))return -1;}s->msw=next;return 1;}
  return -1;
 }
 if(opcode==0xf00){
  unsigned op=o->reg;if(op>5)return rejected(s,failure,6,0);
  if((op==2||op==3)&&cpl)return rejected(s,failure,13,0);
  if(op<2)return write_word(s,c,b,o,op?s->task->tr:s->task->ldtr,failure);
  uint16_t selector;if(!read_word(s,c,b,o,&selector,failure))return 0;
  if(op==2||op==3){
   uint16_t error=0;unsigned vector=op==2?pc98_cpu_lldt286(s->task,s->segments,b,selector,&error):pc98_cpu_ltr286(s->task,s->segments,b,selector,&error);
   return vector?rejected(s,failure,vector,error):1;
  }
  Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 descriptor;uint16_t error=0;
  unsigned vector=pc98_cpu_lookup286(&fetch,&s->segments->tables,selector,&descriptor,&error);
  int valid=!vector&&pc98_cpu_verify_data286(&descriptor,selector,cpl,op==5);
  c->flags=(uint16_t)((c->flags&~PC98_ZF)|(valid?PC98_ZF:0));return 1;
 }
 return -1;
}
