/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "menu_cpu.h"
#include "purge.h"
#include <assert.h>
typedef struct {const Pc98ExecuteBus *bus;uint32_t address[15];uint8_t bytes[15];unsigned count,index;} FetchReplay;
static uint8_t underlying(const Pc98ExecuteBus *b,uint32_t a){return b->fetch8?b->fetch8(b->fetch_context,a):b->read8(b->context,a);}
static uint8_t record(void *ctx,uint32_t a){FetchReplay *r=ctx;assert(r->count<15);uint8_t v=underlying(r->bus,a);r->address[r->count]=a;r->bytes[r->count++]=v;return v;}
static uint8_t replay(void *ctx,uint32_t a){FetchReplay *r=ctx;if(r->index<r->count){assert(a==r->address[r->index]);return r->bytes[r->index++];}return underlying(r->bus,a);}
static unsigned extension_opcode(unsigned op){return (op>=0x80&&op<=0x9f)||op==0xa0||op==0xa1||op==0xa8||op==0xa9||op==0x20||op==0x22||op==0xa3||op==0xab||op==0xb3||op==0xbb||op==0xba||op==0xa4||op==0xa5||op==0xac||op==0xad||op==0xaf||op==0xb2||op==0xb4||op==0xb5||op==0xb6||op==0xb7||op==0xbe||op==0xbf||op==0xbc||op==0xbd;}
/* Only operations that cannot change CPL or switch tasks may use the initial
 * protected hybrid dispatcher. Unhandled transitions must stop before operand
 * data or descriptors; explicit286 task/IRET APIs remain separate. */
static unsigned protected_opcode(unsigned op,unsigned second,unsigned modrm){
 if(op==0x0f)return extension_opcode(second);
 if(op==0xff){unsigned group=(modrm>>3)&7u;return group==0||group==1||group==2||group==4||group==6;}
 return op<=0x3fu||(op>=0x40u&&op<=0x99u)||(op>=0x9bu&&op<=0xc9u)||(op>=0xd0u&&op<=0xe9u)||(op>=0xebu&&op<=0xefu)||(op>=0xf4u&&op<=0xfeu);
}

enum Pc98StepResult pc98_cpu_menu_step(unsigned menu,Pc98Mode286 *m,Pc98Cpu386State *s,const Pc98ExecuteBus *io){
 if(io->instruction_completed)*io->instruction_completed=0;
 Pc98Cpu16State *c=&s->low;
 if(menu>2||c->model!=(menu?PC98_CPU_286:PC98_CPU_V30))return PC98_STEP_UNSUPPORTED;
 if(menu==0)return pc98_cpu_step16(c,io);
 if(menu==1)return pc98_cpu_mode_step286(m,c,io);
 if(m->shutdown)return PC98_STEP_SHUTDOWN;
 if(c->fault.valid)return PC98_STEP_FAULT;
 if(c->halted)return PC98_STEP_HALTED;
 if(PC98_V86(s))return PC98_STEP_UNSUPPORTED;
 unsigned protected_mode=m->system.msw&1u;
 Pc98ExecuteBus b=pc98_cpu_mode_bus_extension386(m,c,io);Pc98ExecuteBus original=b;FetchReplay r={.bus=&original};Pc98Cpu16State probe=*c;unsigned consumed=0;uint32_t cursor=c->ip;
 Pc98FetchBus f={.context=&r,.read8=record,.resolve=b.resolve,.resolve_context=b.resolve_context,.cursor=&cursor,.consumed=&consumed,.maximum=15,.length_has_error=(uint8_t)protected_mode};unsigned op,ext=0,second=0,requested=0,modrm=0,repeat=0;
 for(;;){op=pc98_cpu_fetch8(&probe,&f);if(probe.fault.valid){probe.fault.restart_ip=c->ip;c->fault=probe.fault;return PC98_STEP_FAULT;}
  if(op==0x66||op==0x67||op==0x64||op==0x65){ext=1;requested|=op==0x66?PC98_IGNORE_OPERAND386:op==0x67?PC98_IGNORE_ADDRESS386:PC98_IGNORE_SEGMENT386;continue;}
  if(op==0xf2||op==0xf3){repeat=1;continue;}
  if(op==0x26||op==0x2e||op==0x36||op==0x3e)continue;
  break;
 }
 if(requested&&!(requested&~pc98_cpu_native_prefix_mask386(op)))ext=0;
 /* REP/REPNE before a non-string instruction is ignored (see execute386.c); only the extension path does that. */
 if(repeat&&!((op>=0x6cu&&op<=0x6fu)||(op>=0xa4u&&op<=0xa7u)||(op>=0xaau&&op<=0xafu)))ext=1;
 if(op==0x86||op==0x87||op==0x9c||op==0x9d||op==0xcf||op==0xc0||op==0xc1||(op>=0xd0&&op<=0xd3))ext=1;
 if(op==0x8c||op==0x8e||(protected_mode&&op==0xff)){modrm=pc98_cpu_fetch8(&probe,&f);if(probe.fault.valid){probe.fault.restart_ip=c->ip;c->fault=probe.fault;return PC98_STEP_FAULT;}if(op!=0xff&&((modrm>>3)&7u)>=4)ext=1;}
 if(op==0x0f){second=pc98_cpu_fetch8(&probe,&f);if(probe.fault.valid){probe.fault.restart_ip=c->ip;c->fault=probe.fault;return PC98_STEP_FAULT;}ext|=extension_opcode(second);}
 if(protected_mode){
  if(!protected_opcode(op,second,modrm))return PC98_STEP_UNSUPPORTED;
  unsigned port=(op>=0x6c&&op<=0x6f)||(op>=0xe4&&op<=0xe7)||(op>=0xec&&op<=0xef);
  if(port&&pc98_cpu_cpl16(c)>((c->flags>>12)&3u))return PC98_STEP_UNSUPPORTED;
 }
 if(!ext&&!protected_mode){
  unsigned push=(op>=0x50&&op<=0x57)||op==0x06||op==0x0e||op==0x16||op==0x1e||op==0x68||op==0x6a||op==0xe8;
  unsigned frame=op==0x9a?4u:op==0x60?16u:push?2u:0u;
  if(frame&&c->r[PC98_SP]>0&&c->r[PC98_SP]<frame)return PC98_STEP_UNSUPPORTED;
 }
 b.ignored_prefixes386=(uint8_t)(ext?0u:requested);
 unsigned complete=0;b.fetch8=replay;b.fetch_context=&r;b.instruction_maximum=15;b.instruction_completed=&complete;
 enum Pc98StepResult result=ext?pc98_cpu_extension386(s,&b):pc98_cpu_step16(c,&b);
 if(io->instruction_completed)*io->instruction_completed=complete;
 if(!ext&&complete&&op!=0x9d)s->flag_high&=(uint16_t)~1u;
 return result;
}
