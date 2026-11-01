/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "alu.h"
#include <assert.h>
uint16_t pc98_cpu_alu(enum Pc98AluOp op,unsigned bits,uint16_t left,
                      uint16_t right,uint16_t *flags) {
 assert(bits==8 || bits==16);
 assert(op>=PC98_ADD && op<=PC98_NEG);
 const uint32_t mask=(1u<<bits)-1, sign=1u<<(bits-1);
 uint32_t a=left&mask,b=right&mask,carry=0,result,wide;
 uint16_t changed=PC98_CF|PC98_PF|PC98_AF|PC98_ZF|PC98_SF|PC98_OF;
 uint16_t next=0;
 if(op==PC98_OR || op==PC98_AND || op==PC98_XOR) {
  result=op==PC98_OR ? a|b : op==PC98_AND ? a&b : a^b;
  changed&=(uint16_t)~PC98_AF;
 } else {
  unsigned subtract=op==PC98_SUB || op==PC98_SBB || op==PC98_CMP || op==PC98_DEC || op==PC98_NEG;
  if(op==PC98_ADC || op==PC98_SBB) carry=(*flags&PC98_CF)!=0;
  if(op==PC98_INC || op==PC98_DEC) { b=1;changed&=(uint16_t)~PC98_CF; }
  if(op==PC98_NEG) { b=a;a=0; }
  if(subtract) {
   result=(a-b-carry)&mask;
   if(a<b+carry) next|=PC98_CF;
   if((a^b)&(a^result)&sign) next|=PC98_OF;
  } else {
   wide=a+b+carry;result=wide&mask;
   if(wide>mask) next|=PC98_CF;
   if(~(a^b)&(a^result)&sign) next|=PC98_OF;
  }
  if((a^b^result)&16) next|=PC98_AF;
 }
 if(!result) next|=PC98_ZF;
 if(result&sign) next|=PC98_SF;
 unsigned parity=result&255;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
 if(!(parity&1)) next|=PC98_PF;
 *flags=(*flags&~changed)|(next&changed);
 return (uint16_t)result;
}

uint16_t pc98_cpu_shift(unsigned op,unsigned bits,uint16_t value,unsigned count,uint16_t *flags) {
 assert((bits==8 || bits==16) && op<8 && op!=6 && count<=255);
 uint32_t mask=(1u<<bits)-1,sign=1u<<(bits-1),v=value&mask,initial=v;
 unsigned carry=(*flags&PC98_CF)!=0;
 if(!count)return (uint16_t)v;
 for(unsigned n=0;n<count;n++) {
  unsigned next;
  if(op==0 || op==2 || op==4) {
   next=(v&sign)!=0;
   v=((v<<1)|(op==0?next:op==2?carry:0))&mask;
  } else {
   next=v&1u;
   v=(v>>1)|(op==1?(next?sign:0):op==3?(carry?sign:0):op==7?(v&sign):0);
  }
  carry=next;
 }
 uint16_t changed=PC98_CF,next=(uint16_t)carry;
 if(op>=4) {
  changed|=PC98_ZF|PC98_SF|PC98_PF;
  if(!v)next|=PC98_ZF;
  if(v&sign)next|=PC98_SF;
  unsigned parity=v&255;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
  if(!(parity&1))next|=PC98_PF;
 }
 if(count==1) {
  unsigned overflow;
  if(op==0 || op==2 || op==4)overflow=((v&sign)!=0)^carry;
  else if(op==1 || op==3)overflow=((v&sign)!=0)^((v&(sign>>1))!=0);
  else overflow=op==5 && (initial&sign);
  changed|=PC98_OF;if(overflow)next|=PC98_OF;
 }
 *flags=(uint16_t)((*flags&~changed)|(next&changed));return (uint16_t)v;
}

static int64_t signed_value(uint32_t value,unsigned bits) {
 return (value&(1u<<(bits-1)))?(int64_t)value-((int64_t)1<<bits):(int64_t)value;
}
int pc98_cpu_muldiv(unsigned op,unsigned bits,uint16_t ax,uint16_t dx,
                    uint16_t operand,uint16_t *flags,uint16_t *out_ax,uint16_t *out_dx) {
 assert((bits==8 || bits==16) && op>=4 && op<=7);
 uint32_t mask=(1u<<bits)-1,b=operand&mask;
 uint16_t next_ax=ax,next_dx=dx,next_flags=*flags;
 if(op<6) {
  int64_t product=op==4?(int64_t)(ax&mask)*b:signed_value(ax&mask,bits)*signed_value(b,bits);
  uint32_t raw=(uint32_t)product;
  next_ax=(uint16_t)raw;if(bits==16)next_dx=(uint16_t)(raw>>16);
  int overflow=op==4?(raw>>bits)!=0:product!=signed_value(raw&mask,bits);
  next_flags=(uint16_t)((next_flags&~(PC98_CF|PC98_OF))|(overflow?(PC98_CF|PC98_OF):0));
 } else {
  if(!b)return 0;
  uint32_t dividend=bits==8?ax:((uint32_t)dx<<16)|ax;
  int64_t numerator=op==6?(int64_t)dividend:signed_value(dividend,2*bits);
  int64_t divisor=op==6?(int64_t)b:signed_value(b,bits);
  int64_t quotient=numerator/divisor,remainder=numerator%divisor;
  if(op==6?(quotient>(int64_t)mask):(quotient<-((int64_t)1<<(bits-1)) || quotient>=((int64_t)1<<(bits-1))))return 0;
  if(bits==8)next_ax=(uint16_t)(((uint32_t)quotient&255u)|(((uint32_t)remainder&255u)<<8));
  else {next_ax=(uint16_t)quotient;next_dx=(uint16_t)remainder;}
 }
 *out_ax=next_ax;*out_dx=next_dx;*flags=next_flags;return 1;
}

uint16_t pc98_cpu_adjust286(unsigned opcode,uint16_t ax,uint16_t *flags) {
 assert(opcode==0x27 || opcode==0x2f || opcode==0x37 || opcode==0x3f || opcode==0xd4 || opcode==0xd5);
 unsigned al=ax&255u,ah=ax>>8,old=al,carry=(*flags&PC98_CF)!=0,aux=(*flags&PC98_AF)!=0;
 uint16_t changed=0,next=0;
 if(opcode==0x27 || opcode==0x2f) {
  unsigned adjust=(al&15u)>9 || aux,subtract=opcode==0x2f;
  unsigned low_carry=carry;
  if(adjust) {
   low_carry|=subtract?al<6:al>249;
   al=(al+(subtract?250u:6u))&255u;next|=PC98_AF;
  }
  if(old>0x99 || carry) {al=(al+(subtract?160u:96u))&255u;next|=PC98_CF;}
  else if(subtract && low_carry)next|=PC98_CF;
  changed=PC98_CF|PC98_AF|PC98_SF|PC98_ZF|PC98_PF;
 } else if(opcode==0x37 || opcode==0x3f) {
  if((al&15u)>9 || aux) {
   unsigned subtract=opcode==0x3f;
   al=(al+(subtract?250u:6u))&255u;ah=(ah+(subtract?255u:1u))&255u;
   next=PC98_AF|PC98_CF;
  }
  al&=15;changed=PC98_AF|PC98_CF;
 } else {
  uint16_t result;(void)pc98_cpu_adjust_base286(opcode,ax,10,flags,&result);return result;
 }
 if(changed&PC98_ZF) {
  if(!al)next|=PC98_ZF;
  if(al&128u)next|=PC98_SF;
  unsigned parity=al;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
  if(!(parity&1))next|=PC98_PF;
 }
 *flags=(uint16_t)((*flags&~changed)|(next&changed));return (uint16_t)((ah<<8)|al);
}

uint16_t pc98_cpu_adjust_v30(unsigned opcode,uint16_t ax,uint16_t *flags) {
 uint16_t result=pc98_cpu_adjust286(opcode,ax,flags);
 if(opcode==0x27) {
  unsigned before=ax&255u,after=result&255u,correction=(after-before)&255u;
  unsigned overflow=(~(before^correction)&(before^after)&128u)!=0;
  *flags=(uint16_t)((*flags&~PC98_OF)|(overflow?PC98_OF:0));
 } else if(opcode==0xd4) {
  *flags=(uint16_t)((*flags&~PC98_ZF)|(result?0:PC98_ZF));
 }
 return result;
}

int pc98_cpu_adjust_base286(unsigned opcode,uint16_t ax,uint8_t base,uint16_t *flags,uint16_t *out) {
 assert(opcode==0xd4 || opcode==0xd5);
 unsigned al=ax&255u,ah=ax>>8;
 if(opcode==0xd4) {
  if(!base)return 0;
  ah=al/base;al%=base;
 } else {al=(al+ah*base)&255u;ah=0;}
 uint16_t next=0;
 if(!al)next|=PC98_ZF;
 if(al&128u)next|=PC98_SF;
 unsigned parity=al;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
 if(!(parity&1))next|=PC98_PF;
 *flags=(uint16_t)((*flags&~(PC98_ZF|PC98_SF|PC98_PF))|next);
 *out=(uint16_t)((ah<<8)|al);return 1;
}
