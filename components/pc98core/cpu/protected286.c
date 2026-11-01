/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "protected286.h"
#include <assert.h>
unsigned pc98_cpu_load_segment286(Pc98Cpu16State *c,Pc98ProtectedSegments286 *p,
 const Pc98ExecuteBus *b,unsigned segment,uint16_t selector,uint16_t *error) {
 assert(c->model==PC98_CPU_286 && segment<4 && segment!=PC98_CS);
 if(!(selector&0xfffcu)) {
  if(segment==PC98_SS) {*error=0;return 13;}
  c->seg[segment]=selector;c->base[segment]=0;p->valid[segment]=0;
  p->cache[segment]=(Pc98Descriptor286){0};*error=0;return 0;
 }
 const Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 descriptor;
 unsigned fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&descriptor,error);
 if(fault)return fault;
 fault=pc98_cpu_load_data286(&descriptor,selector,pc98_cpu_cpl16(c),segment==PC98_SS,error);
 if(fault)return fault;
 if(!(descriptor.access&1)) {
  const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
  descriptor.access|=1;
  b->write8(b->context,(table->base+(selector&0xfff8u)+5u)&0xffffffu,descriptor.access);
 }
 c->seg[segment]=selector;c->base[segment]=descriptor.base;
 p->cache[segment]=descriptor;p->valid[segment]=1;return 0;
}
unsigned pc98_cpu_jump_code286(Pc98Cpu16State *c,Pc98ProtectedSegments286 *p,
 const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,uint16_t *error) {
 assert(c->model==PC98_CPU_286);
 const Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 descriptor;
 unsigned fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&descriptor,error);
 if(fault)return fault;
 if(!(descriptor.access&16))return PC98_PROTECTED_SYSTEM_PENDING;
 unsigned cpl=pc98_cpu_cpl16(c);
 fault=pc98_cpu_check_code286(&descriptor,selector,cpl,offset,error);
 if(fault)return fault;
 if(!(descriptor.access&1)) {
  const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
  descriptor.access|=1;
  b->write8(b->context,(table->base+(selector&0xfff8u)+5u)&0xffffffu,descriptor.access);
 }
 c->seg[PC98_CS]=(uint16_t)((selector&0xfffcu)|cpl);c->base[PC98_CS]=descriptor.base;c->ip=offset;
 p->cache[PC98_CS]=descriptor;p->valid[PC98_CS]=1;return 0;
}
unsigned pc98_cpu_translate286(const Pc98ProtectedSegments286 *p,unsigned segment,
 uint16_t offset,uint32_t size,enum Pc98SegmentAccess286 access,uint32_t *physical) {
 assert(segment<4 && access>=PC98_SEG_READ && access<=PC98_SEG_EXECUTE);
 if(!p->valid[segment])return 13;
 const Pc98Descriptor286 *d=&p->cache[segment];unsigned ar=d->access;
 if(!(ar&16))return 13;
 if(access==PC98_SEG_EXECUTE) {if(!(ar&8)&&!(segment==PC98_CS&&(ar&0x1eu)==0x12u))return 13;}
 else if(access==PC98_SEG_WRITE) {if((ar&8)||!(ar&2))return 13;}
 else if((ar&8)&&!(ar&2))return 13;
 if(!pc98_cpu_range286(d,offset,size))return segment==PC98_SS?12u:13u;
 *physical=(d->base+offset)&0xffffffu;return 0;
}

int pc98_cpu_resolve_segment286(void *context,unsigned segment,uint32_t offset,uint32_t size,
 enum Pc98MemoryAccess16 access,uint32_t *physical,Pc98CpuFault16 *fault) {
 const Pc98ProtectedSegments286 *p=context;
 enum Pc98SegmentAccess286 purpose=access==PC98_ACCESS_FETCH?PC98_SEG_EXECUTE:access==PC98_ACCESS_WRITE?PC98_SEG_WRITE:PC98_SEG_READ;
 unsigned vector=offset>0xffffu?(segment==PC98_SS?12u:13u):pc98_cpu_translate286(p,segment,(uint16_t)offset,size,purpose,physical);
 if(!vector)return 1;
 *fault=(Pc98CpuFault16){.vector=(uint8_t)vector,.valid=1,.has_error=1};return 0;
}

int pc98_cpu_load_segment_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,
 unsigned segment,uint16_t selector,Pc98CpuFault16 *fault) {
 uint16_t error=0;unsigned vector=pc98_cpu_load_segment286(c,context,b,segment,selector,&error);
 if(!vector)return 1;
 *fault=(Pc98CpuFault16){.vector=(uint8_t)vector,.valid=1,.has_error=1,.error=error};return 0;
}

int pc98_cpu_jump_far_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,
 uint16_t selector,uint16_t offset,Pc98CpuFault16 *fault) {
 uint16_t error=0;unsigned vector=pc98_cpu_jump_code286(c,context,b,selector,offset,&error);
 if(!vector)return 1;
 if(vector==PC98_PROTECTED_SYSTEM_PENDING)return -1;
 *fault=(Pc98CpuFault16){.vector=(uint8_t)vector,.valid=1,.has_error=1,.error=error};return 0;
}

unsigned pc98_cpu_call_code286(Pc98Cpu16State *c,Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,uint16_t *error) {
 assert(c->model==PC98_CPU_286);Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 code;
 unsigned fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&code,error);if(fault)return fault;
 if(!(code.access&16))return PC98_PROTECTED_SYSTEM_PENDING;
 unsigned cpl=pc98_cpu_cpl16(c);Pc98Descriptor286 probe=code;probe.limit=0xffff;
 fault=pc98_cpu_check_code286(&probe,selector,cpl,0,error);if(fault)return fault;
 uint16_t lowest=(uint16_t)(c->r[PC98_SP]-4);uint32_t physical;
 fault=pc98_cpu_translate286(p,PC98_SS,lowest,4,PC98_SEG_WRITE,&physical);if(fault){*error=0;return fault;}
 if(offset>code.limit){*error=0;return 13;}
 if(!(code.access&1)){
  const Pc98Table286 *table=(selector&4u)?&p->tables.ldt:&p->tables.gdt;
  b->write8(b->context,(table->base+(selector&0xfff8u)+5u)&0xffffffu,(uint8_t)(code.access|1));
 }
 b->write16(b->context,(physical+2)&0xffffffu,c->seg[PC98_CS]);b->write16(b->context,physical,c->ip);
 code.access|=1;p->cache[PC98_CS]=code;p->valid[PC98_CS]=1;
 c->seg[PC98_CS]=(uint16_t)((selector&0xfffcu)|cpl);c->base[PC98_CS]=code.base;c->ip=offset;c->r[PC98_SP]=lowest;*error=0;return 0;
}
int pc98_cpu_call_far_bridge286(void *context,Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,Pc98CpuFault16 *failure) {
 uint16_t error=0;unsigned fault=pc98_cpu_call_code286(c,context,b,selector,offset,&error);
 if(!fault)return 1;
 if(fault==PC98_PROTECTED_SYSTEM_PENDING)return -1;
 *failure=(Pc98CpuFault16){.valid=1,.has_error=1,.vector=(uint8_t)fault,.error=error};return 0;
}
