/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "muldiv386.h"
#include "alu.h"
#include <assert.h>
#include <limits.h>
static int64_t signed32(uint32_t value){return value<UINT32_C(0x80000000)?(int64_t)value:(int64_t)value-INT64_C(0x100000000);}
int pc98_cpu_muldiv386(unsigned op,uint32_t ax,uint32_t dx,uint32_t operand,uint32_t *flags,uint32_t *out_ax,uint32_t *out_dx){
 assert(op>=4&&op<=7);
 uint32_t low,high;
 if(op<6){
  uint64_t product;unsigned overflow;
  if(op==4){product=(uint64_t)ax*operand;overflow=(product>>32)!=0;}
  else{int64_t signed_product=signed32(ax)*signed32(operand);product=(uint64_t)signed_product;overflow=signed_product<INT32_MIN||signed_product>INT32_MAX;}
  low=(uint32_t)product;high=(uint32_t)(product>>32);
  *flags=(*flags&~(uint32_t)(PC98_CF|PC98_OF))|(overflow?(PC98_CF|PC98_OF):0u);
 }else{
  if(!operand)return 0;
  uint64_t combined=((uint64_t)dx<<32)|ax;
  if(op==6){uint64_t quotient=combined/operand;if(quotient>UINT32_MAX)return 0;low=(uint32_t)quotient;high=(uint32_t)(combined%operand);}
  else{
   int64_t dividend=dx&UINT32_C(0x80000000)?-1-(int64_t)~combined:(int64_t)combined,divisor=signed32(operand);
   if(dividend==INT64_MIN&&divisor==-1)return 0;
   int64_t quotient=dividend/divisor;if(quotient<INT32_MIN||quotient>INT32_MAX)return 0;
   low=(uint32_t)quotient;high=(uint32_t)(dividend%divisor);
  }
 }
 *out_ax=low;*out_dx=high;return 1;
}
