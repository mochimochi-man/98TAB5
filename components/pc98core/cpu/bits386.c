/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "bits386.h"
#include "alu.h"
#include <assert.h>
static uint32_t mask_for(unsigned bits){assert(bits==16||bits==32);return bits==16?0xffffu:UINT32_MAX;}
uint32_t pc98_cpu_bit386(enum Pc98Bit386 op,unsigned bits,uint32_t value,unsigned index,uint32_t *flags){
 assert(op>=PC98_BIT_TEST386&&op<=PC98_BIT_COMPLEMENT386);
 uint32_t mask=mask_for(bits),bit=UINT32_C(1)<<(index&(bits-1));value&=mask;
 *flags=(*flags&~(uint32_t)PC98_CF)|((value&bit)?PC98_CF:0u);
 switch(op){case PC98_BIT_SET386:value|=bit;break;case PC98_BIT_RESET386:value&=~bit;break;case PC98_BIT_COMPLEMENT386:value^=bit;break;default:break;}
 return value;
}
int pc98_cpu_scan386(unsigned reverse,unsigned bits,uint32_t source,uint32_t *destination,uint32_t *flags){
 assert(reverse<2);source&=mask_for(bits);
 *flags=(*flags&~(uint32_t)PC98_ZF)|(source?0u:PC98_ZF);if(!source)return 0;
 unsigned index=reverse?bits-1:0;
 while(!(source&(UINT32_C(1)<<index))){if(reverse)index--;else index++;}
 *destination=index;return 1;
}
int pc98_cpu_double_shift386(unsigned right,unsigned bits,uint32_t destination,uint32_t source,unsigned count,uint32_t *flags,uint32_t *result){
 assert(right<2);uint32_t mask=mask_for(bits);count&=31u;
 if(!count){*result=destination&mask;return 1;}
 if(count>=bits)return 0;
 destination&=mask;source&=mask;
 uint64_t pair=right?((uint64_t)source<<bits)|destination:((uint64_t)destination<<bits)|source;
 uint32_t value=(uint32_t)(right?pair>>count:(pair<<count)>>bits)&mask;
 unsigned carry=(destination>>(right?count-1:bits-count))&1u;
 unsigned parity=0;for(unsigned i=0;i<8;i++)parity^=(value>>i)&1u;
 *flags=(*flags&~(uint32_t)(PC98_CF|PC98_PF|PC98_ZF|PC98_SF))|(carry?PC98_CF:0u)|(!parity?PC98_PF:0u)|(!value?PC98_ZF:0u)|((value>>(bits-1))?PC98_SF:0u);
 *result=value;return 1;
}

int pc98_cpu_bit_address386(unsigned bits,unsigned address_bits,uint32_t base,uint32_t index,unsigned immediate,uint32_t *address,unsigned *bit){
 assert((bits==16||bits==32)&&(address_bits==16||address_bits==32)&&immediate<2);
 int64_t signed_index=0,word=0;
 if(!immediate){signed_index=bits==16?(int64_t)(index&0xffffu):(int64_t)index;if(signed_index>=(INT64_C(1)<<(bits-1)))signed_index-=INT64_C(1)<<bits;word=signed_index<0?(signed_index-(bits-1))/bits:signed_index/bits;}
 int64_t adjusted=(int64_t)base+word*(bits/8);uint32_t maximum=address_bits==16?0xffffu:UINT32_MAX;
 if(adjusted<0||adjusted>maximum)return 0;
 *address=(uint32_t)adjusted;*bit=immediate?index&(bits-1):(unsigned)(signed_index-word*bits);return 1;
}
