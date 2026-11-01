/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel Undocumented iAPX 286 Test Instruction pp.2-4: real hidden caches
 * survive LOADALL; explicit real segment reload restores paragraph mapping. */
#include "real286.h"
#include "mode286.h"
/* Every cache except a readable-code CS keeps its access-rights checks in real mode (a LOADALL read-only
 * cache faults on writes). CS with the readable-code cache every real CS load produces
 * checks only the hidden limit: DOS writes and reads through CS: overrides. */
int pc98_cpu_real_resolve286(void *context,unsigned segment,uint32_t offset,uint32_t size,enum Pc98MemoryAccess16 access,uint32_t *physical,Pc98CpuFault16 *fault){
 const Pc98ProtectedSegments286 *p=context;
 if(p->valid[segment]&&(segment!=PC98_CS||(p->cache[segment].access&0x0au)!=0x0au)){
  int result=pc98_cpu_resolve_segment286(context,segment,offset,size,access,physical,fault);if(!result)fault->has_error=0;return result;
 }
 if(offset>0xffffu||!p->valid[segment]||!pc98_cpu_range286(&p->cache[segment],(uint16_t)offset,size)){
  *fault=(Pc98CpuFault16){.vector=(uint8_t)(segment==PC98_SS?12u:13u),.valid=1};return 0;
 }
 *physical=(p->cache[segment].base+offset)&0xffffffu;return 1;
}
int pc98_cpu_real_load286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *bus,unsigned segment,uint16_t selector,Pc98CpuFault16 *fault){
 (void)bus;(void)fault;Pc98Mode286 *m=context;c->seg[segment]=selector;c->base[segment]=(uint32_t)selector<<4;
 m->segments.cache[segment]=(Pc98Descriptor286){.base=c->base[segment],.limit=0xffff,.access=(uint8_t)(segment==PC98_CS?0x9b:0x93)};m->segments.valid[segment]=1;return 1;
}
int pc98_cpu_real_jump286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *bus,uint16_t selector,uint16_t target,Pc98CpuFault16 *fault){
 (void)pc98_cpu_real_load286(context,c,bus,PC98_CS,selector,fault);c->ip=target;return 1;
}
static int stack(const Pc98ExecuteBus *b,uint16_t offset,unsigned size,enum Pc98MemoryAccess16 access,uint32_t *physical,Pc98CpuFault16 *fault){return b->resolve(b->resolve_context,PC98_SS,offset,size,access,physical,fault);}
int pc98_cpu_real_call286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t selector,uint16_t target,Pc98CpuFault16 *fault){
 uint16_t lowest=(uint16_t)(c->r[PC98_SP]-4u);uint32_t physical;if(!stack(b,lowest,4,PC98_ACCESS_WRITE,&physical,fault))return 0;
 b->write16(b->context,(physical+2u)&0xffffffu,c->seg[PC98_CS]);b->write16(b->context,physical,c->ip);c->r[PC98_SP]=lowest;return pc98_cpu_real_jump286(context,c,b,selector,target,fault);
}
static int returning(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t release,int flags,Pc98CpuFault16 *fault){
 unsigned size=flags?6u:4u;uint32_t physical;if(!stack(b,c->r[PC98_SP],size,PC98_ACCESS_READ,&physical,fault))return 0;
 uint16_t ip=b->read16(b->context,physical),cs=b->read16(b->context,(physical+2u)&0xffffffu);
 if(flags){c->flags=(uint16_t)((b->read16(b->context,(physical+4u)&0xffffffu)&0x0fd5u)|2u);c->nmi_active=0;}
 c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+size+release);return pc98_cpu_real_jump286(context,c,b,cs,ip,fault);
}
int pc98_cpu_real_return286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t release,Pc98CpuFault16 *fault){return returning(context,c,b,release,0,fault);}
int pc98_cpu_real_iret286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,Pc98CpuFault16 *fault){return returning(context,c,b,0,1,fault);}
int pc98_cpu_real_interrupt286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned vector,int software,int external,int has_error,uint16_t error,Pc98CpuFault16 *fault){
 (void)software;(void)external;(void)has_error;(void)error;Pc98Mode286 *m=context;
 if(vector>255u||vector*4u+3u>m->interrupts.idt.limit){*fault=(Pc98CpuFault16){.valid=1,.vector=13};return 0;}
 uint16_t lowest=(uint16_t)(c->r[PC98_SP]-6u);uint32_t physical;if(!stack(b,lowest,6,PC98_ACCESS_WRITE,&physical,fault))return 0;
 b->write16(b->context,(physical+4u)&0xffffffu,(uint16_t)((c->flags&0x7fd5u)|2u));b->write16(b->context,(physical+2u)&0xffffffu,c->seg[PC98_CS]);b->write16(b->context,physical,c->ip);
 uint32_t entry=(m->interrupts.idt.base+vector*4u)&0xffffffu;uint16_t target=b->read16(b->context,entry),selector=b->read16(b->context,(entry+2u)&0xffffffu);
 c->r[PC98_SP]=lowest;c->flags&=(uint16_t)~0x300u;c->irq_inhibit=c->nmi_inhibit=c->halted=c->debug_inhibit=0;return pc98_cpu_real_jump286(context,c,b,selector,target,fault);
}
