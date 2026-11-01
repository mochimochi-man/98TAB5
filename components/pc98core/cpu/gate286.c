/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "gate286.h"
#include <assert.h>
Pc98Gate286 pc98_cpu_gate286(const uint8_t bytes[8]) {
 return (Pc98Gate286){.offset=(uint16_t)(bytes[0]|((unsigned)bytes[1]<<8)),
  .selector=(uint16_t)(bytes[2]|((unsigned)bytes[3]<<8)),.access=bytes[5]};
}
static uint16_t idt_error(unsigned vector,int external) {
 return (uint16_t)((vector<<3)|2u|(external?1u:0u));
}
unsigned pc98_cpu_lookup_gate286(const Pc98FetchBus *b,const Pc98Table286 *idt,
 unsigned vector,int external,Pc98Gate286 *out,uint16_t *error) {
 assert(vector<256);uint32_t start=vector*8;
 if(start+7>idt->limit){*error=idt_error(vector,external);return 13;}
 uint8_t bytes[8];for(unsigned i=0;i<8;i++)bytes[i]=b->read8(b->context,(idt->base+start+i)&0xffffffu);
 *out=pc98_cpu_gate286(bytes);*error=0;return 0;
}
unsigned pc98_cpu_check_gate286(const Pc98Gate286 *gate,unsigned vector,unsigned cpl,
 int software,int external,uint16_t *error) {
 assert(vector<256&&cpl<4);unsigned type=gate->access&31u,dpl=(gate->access>>5)&3u;
 *error=idt_error(vector,external);
 if((type!=5&&type!=6&&type!=7)||(software&&cpl>dpl))return 13;
 if(!(gate->access&128u))return 11;
 *error=0;return 0;
}
unsigned pc98_cpu_interrupt_code286(const Pc98Descriptor286 *d,uint16_t selector,
 unsigned cpl,uint16_t offset,unsigned *new_cpl,uint16_t *error) {
 assert(cpl<4);*error=(uint16_t)(selector&0xfffcu);
 if(!(selector&0xfffcu)){*error=0;return 13;}
 unsigned ar=d->access,dpl=(ar>>5)&3u;
 if((ar&24u)!=24u||dpl>cpl)return 13;
 if(!(ar&128u))return 11;
 if(offset>d->limit){*error=0;return 13;}
 *new_cpl=(ar&4u)?cpl:dpl;*error=0;return 0;
}
