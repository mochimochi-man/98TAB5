/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel286 PRM8.3 and SWITCH_TASKS B-12/B-13. */
#include "task_switch286.h"
#include "cpuprof.h"
#include "purge.h"
#include <assert.h>
#include <string.h>
static int fault_record(Pc98Cpu16State *c,Pc98CpuFault16 *out,unsigned vector,uint16_t error,int external,int committed){
 *out=(Pc98CpuFault16){.valid=1,.has_error=1,.committed=(uint8_t)committed,.vector=(uint8_t)vector,.error=(uint16_t)(error|(external?1u:0u)),.restart_ip=c->ip};
 if(committed)c->fault=*out;
 return 0;
}
#if !PC98_FULLCPU
/* Stage 13-2: task switching is purged; reaching it stops the machine (unsupported path). */
int pc98_cpu_switch_task286(Pc98Cpu16State *c,Pc98TaskSwitch286 *context,const Pc98ExecuteBus *b,
 uint16_t selector,enum Pc98TaskTransfer286 mode,int check_privilege,int external,
 int has_error,uint16_t original_error,Pc98CpuFault16 *failure){
 (void)c;(void)context;(void)b;(void)selector;(void)check_privilege;(void)external;(void)has_error;(void)original_error;(void)failure;
 static const char *const kind[]={"TSS task switch (CALL)","TSS task switch (JMP)","TSS task switch (IRET, NT)","TSS task switch (INT via task gate)"};
 PURGE_STOP(kind[(unsigned)mode&3u]);
 return -1;
}
#else
static void mark_accessed(Pc98Descriptor286 *d,const Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,uint16_t selector){
 if(d->access&1)return;
 const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
 d->access|=1;b->write8(b->context,(table->base+(selector&0xfff8u)+5u)&0xffffffu,d->access);
}
int pc98_cpu_switch_task286(Pc98Cpu16State *c,Pc98TaskSwitch286 *context,const Pc98ExecuteBus *b,
 uint16_t selector,enum Pc98TaskTransfer286 mode,int check_privilege,int external,
 int has_error,uint16_t original_error,Pc98CpuFault16 *failure){
 assert(c->model==PC98_CPU_286&&mode>=PC98_TASK_CALL&&mode<=PC98_TASK_INTERRUPT);
 PROF_EVT(PEV_TASK_CALL+(unsigned)mode);
 Pc98ProtectedSegments286 *p=context->segments;Pc98TaskRegisters286 *tr=context->task;
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};uint16_t error=(uint16_t)(selector&0xfffcu);unsigned fault=mode==PC98_TASK_IRET?10u:13u;
 if(!(selector&0xfffcu)||(selector&4u))goto pre_fault;
 Pc98Descriptor286 incoming;unsigned lookup=pc98_cpu_lookup286(&fetch,&p->tables,selector,&incoming,&error);
 if(lookup){fault=mode==PC98_TASK_IRET?10u:13u;goto pre_fault;}
 error=(uint16_t)(selector&0xfffcu);
 if(check_privilege){unsigned dpl=(incoming.access>>5)&3u;if(dpl<pc98_cpu_cpl16(c)||dpl<(selector&3u)){fault=13;goto pre_fault;}}
 if((incoming.access&31u)!=(mode==PC98_TASK_IRET?3u:1u)){fault=mode==PC98_TASK_IRET?10u:13u;goto pre_fault;}
 if(!(incoming.access&128u)){fault=11;goto pre_fault;}
 if(!context->msw||!tr->tr_valid||tr->tss.limit<41||incoming.limit<43||!(tr->tr&0xfffcu)||(tr->tr&4u)||((tr->tr&0xfff8u)==(selector&0xfff8u)))return -1;
 /* The old TSS is saved before incoming fields are read; preserve this
  * order even if their physical ranges alias. LDT selector is static. */
 uint16_t old_tr=tr->tr;Pc98Descriptor286 outgoing=tr->tss;Pc98Cpu16State saved=*c;
 if(mode==PC98_TASK_IRET)saved.flags&=(uint16_t)~0x4000u;
 uint8_t bytes[44]={0};pc98_cpu_save_tss286(bytes,&saved);
 incoming.access|=2;b->write8(b->context,(p->tables.gdt.base+(selector&0xfff8u)+5u)&0xffffffu,incoming.access);
 for(unsigned i=14;i<42;i++)b->write8(b->context,(outgoing.base+i)&0xffffffu,bytes[i]);
 int nesting=mode==PC98_TASK_CALL||mode==PC98_TASK_INTERRUPT;
 if(nesting)b->write16(b->context,incoming.base,old_tr);
 else{uint32_t a=(p->tables.gdt.base+(old_tr&0xfff8u)+5u)&0xffffffu;b->write8(b->context,a,(uint8_t)(b->read8(b->context,a)&~2u));}
 tr->tr=selector;tr->tss=incoming;tr->tr_valid=1;
 for(unsigned i=0;i<44;i++)bytes[i]=b->read8(b->context,(incoming.base+i)&0xffffffu);
 Pc98Tss286 next=pc98_cpu_parse_tss286(bytes);memcpy(c->r,next.r,sizeof(c->r));memcpy(c->seg,next.seg,sizeof(c->seg));pc98_cpu_set_cpl16(c,c->seg[PC98_CS]&3u);c->ip=next.ip;c->flags=(uint16_t)((next.flags&0x7fd5u)|2u|(nesting?0x4000u:0u));
 for(unsigned s=0;s<4;s++){c->base[s]=0;p->cache[s]=(Pc98Descriptor286){0};p->valid[s]=0;}
 p->tables.ldt=(Pc98Table286){0};p->tables.ldt_valid=0;tr->ldtr=next.ldt;
 *context->msw|=8;if(b->task_committed)*b->task_committed=1;c->irq_inhibit=c->nmi_inhibit=c->halted=c->debug_inhibit=0;c->fault=(Pc98CpuFault16){0};
 /* All subsequent faults belong to the incoming task; no outgoing rollback. */
 if(next.ldt&0xfffcu){
  error=(uint16_t)(next.ldt&0xfffcu);fault=10;if(next.ldt&4u)goto post_fault;
  Pc98Descriptor286 ldt;lookup=pc98_cpu_lookup286(&fetch,&p->tables,next.ldt,&ldt,&error);if(lookup){fault=10;goto post_fault;}
  error=(uint16_t)(next.ldt&0xfffcu);if((ldt.access&31u)!=2||!(ldt.access&128u))goto post_fault;
  p->tables.ldt=(Pc98Table286){ldt.base,ldt.limit};p->tables.ldt_valid=1;
 }
 fault=pc98_cpu_load_segment286(c,p,b,PC98_SS,c->seg[PC98_SS],&error);if(fault){if(fault==13)fault=10;goto post_fault;}
 Pc98Descriptor286 code;uint16_t cs=c->seg[PC98_CS];unsigned cpl=cs&3u;
 lookup=pc98_cpu_lookup286(&fetch,&p->tables,cs,&code,&error);if(lookup){fault=10;goto post_fault;}
 Pc98Descriptor286 probe=code;probe.limit=0xffff;
 fault=pc98_cpu_check_code286(&probe,cs,cpl,0,&error);if(fault){if(fault==13)fault=10;goto post_fault;}
 mark_accessed(&code,p,b,cs);p->cache[PC98_CS]=code;p->valid[PC98_CS]=1;c->base[PC98_CS]=code.base;
 const unsigned data_segments[]={PC98_DS,PC98_ES};
 for(unsigned n=0;n<2;n++){unsigned segment=data_segments[n];fault=pc98_cpu_load_segment286(c,p,b,segment,c->seg[segment],&error);if(fault){if(fault==13)fault=10;goto post_fault;}}
 if(c->ip>code.limit){fault=13;error=0;goto post_fault;}
 if(has_error){uint16_t lowest=(uint16_t)(c->r[PC98_SP]-2);uint32_t physical;fault=pc98_cpu_translate286(p,PC98_SS,lowest,2,PC98_SEG_WRITE,&physical);if(fault){error=0;goto post_fault;}b->write16(b->context,physical,original_error);c->r[PC98_SP]=lowest;}
 if(mode==PC98_TASK_IRET)c->nmi_active=0;
 return 1;
pre_fault:
 return fault_record(c,failure,fault,error,external,0);
post_fault:
 return fault_record(c,failure,fault,error,external,1);
}

#endif
int pc98_cpu_jump_task_bridge286(void *opaque,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,Pc98CpuFault16 *failure){
 Pc98TaskSwitch286 *context=opaque;Pc98ProtectedSegments286 *p=context->segments;uint16_t error=0;
 unsigned fault=pc98_cpu_jump_code286(c,p,b,selector,offset,&error);
 if(!fault)return 1;
 if(fault!=PC98_PROTECTED_SYSTEM_PENDING)return fault_record(c,failure,fault,error,0,0);
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 gate;
 fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&gate,&error);if(fault)return fault_record(c,failure,fault,error,0,0);
 unsigned type=gate.access&31u;
 if(type==1||type==3)return pc98_cpu_switch_task286(c,context,b,selector,PC98_TASK_JUMP,1,0,0,0,failure);
 unsigned cpl=pc98_cpu_cpl16(c),dpl=(gate.access>>5)&3u;error=(uint16_t)(selector&0xfffcu);
 if((type!=4&&type!=5)||dpl<cpl||dpl<(selector&3u))return fault_record(c,failure,13,error,0,0);
 if(!(gate.access&128u))return fault_record(c,failure,11,error,0,0);
 uint16_t target=(uint16_t)gate.base;
 if(type==5)return pc98_cpu_switch_task286(c,context,b,target,PC98_TASK_JUMP,0,0,0,0,failure);
 fault=pc98_cpu_jump_code286(c,p,b,(uint16_t)(target&0xfffcu),gate.limit,&error);
 if(!fault)return 1;
 if(fault==PC98_PROTECTED_SYSTEM_PENDING){fault=13;error=(uint16_t)(target&0xfffcu);}
 return fault_record(c,failure,fault,error,0,0);
}
