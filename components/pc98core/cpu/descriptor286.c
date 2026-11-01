/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "descriptor286.h"
#include <assert.h>
Pc98Descriptor286 pc98_cpu_descriptor286(const uint8_t b[8]) {
 Pc98Descriptor286 d;
 d.limit=(uint16_t)(b[0]|((unsigned)b[1]<<8));
 d.base=b[2]|((uint32_t)b[3]<<8)|((uint32_t)b[4]<<16);d.access=b[5];
 /* Bytes 6/7 are reserved on 286, not a 386 limit/base extension. */
 return d;
}
unsigned pc98_cpu_lookup286(const Pc98FetchBus *b,const Pc98DescriptorTables286 *tables,
                           uint16_t selector,Pc98Descriptor286 *out,uint16_t *error) {
 *error=selector&0xfffcu;
 if(!*error)return 13;
 const Pc98Table286 *table=(selector&4u)?&tables->ldt:&tables->gdt;
 if((selector&4u)&&!tables->ldt_valid)return 13;
 unsigned offset=selector&0xfff8u;
 if(offset+7u>table->limit)return 13;
 uint8_t bytes[8];
 for(unsigned n=0;n<8;n++)bytes[n]=b->read8(b->context,(table->base+offset+n)&0xffffffu);
 *out=pc98_cpu_descriptor286(bytes);*error=0;return 0;
}
unsigned pc98_cpu_load_data286(const Pc98Descriptor286 *d,uint16_t selector,unsigned cpl,int stack,uint16_t *error) {
 assert(cpl<4);
 *error=selector&0xfffcu;
 if(!(selector&0xfffcu)) {*error=0;return stack?13u:0u;}
 if(!d)return 13;
 unsigned a=d->access,rpl=selector&3u,dpl=(a>>5)&3u;
 if(!(a&16))return 13;
 if(stack) {
  if((a&8)||!(a&2)||rpl!=cpl||dpl!=cpl)return 13;
 } else {
  if((a&8)&&!(a&2))return 13;
  if(!((a&8)&&(a&4))&&(cpl>dpl||rpl>dpl))return 13;
 }
 if(!(a&128))return stack?12u:11u;
 *error=0;return 0;
}
unsigned pc98_cpu_check_code286(const Pc98Descriptor286 *d,uint16_t selector,unsigned cpl,uint16_t offset,uint16_t *error) {
 assert(cpl<4);*error=selector&0xfffcu;
 if(!*error||!d)return 13;
 unsigned a=d->access,dpl=(a>>5)&3u;
 if((a&0x18u)!=0x18u)return 13;
 if(a&4) {if(dpl>cpl)return 13;}
 else if((selector&3u)>cpl||dpl!=cpl)return 13;
 if(!(a&128))return 11;
 *error=0;if(offset>d->limit)return 13;
 return 0;
}
int pc98_cpu_range286(const Pc98Descriptor286 *d,uint16_t offset,uint32_t size) {
 if(!size||size>0x10000u-offset)return 0;
 uint32_t end=offset+size-1;
 if((d->access&0x1cu)==0x14u)return offset>d->limit;
 return end<=d->limit;
}
