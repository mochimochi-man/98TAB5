/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "shift386.h"
#include "alu.h"
#include <assert.h>
uint32_t pc98_cpu_shift386(unsigned op,uint32_t value,unsigned count,uint32_t *flags){
 assert(op<8&&op!=6);count&=31u;if(!count)return value;
 uint32_t old=*flags,result,carry;
 if(op<2){result=op?((value>>count)|(value<<(32-count))):((value<<count)|(value>>(32-count)));carry=op?result>>31:result&1u;}
 else if(op<4){uint64_t pair=((uint64_t)(old&PC98_CF)<<32)|value,mask=(UINT64_C(1)<<33)-1;
  pair=(op==2?((pair<<count)|(pair>>(33-count))):((pair>>count)|(pair<<(33-count))))&mask;
  result=(uint32_t)pair;carry=(uint32_t)(pair>>32);
 }else{carry=(value>>(op==4?32-count:count-1))&1u;
  result=op==4?value<<count:value>>count;if(op==7&&(value>>31))result|=UINT32_MAX<<(32-count);
 }
 uint32_t changed=PC98_CF,status=carry;
 if(op>=4){unsigned parity=0;for(unsigned i=0;i<8;i++)parity^=(result>>i)&1u;changed|=PC98_PF|PC98_ZF|PC98_SF;status|=(!parity?PC98_PF:0u)|(!result?PC98_ZF:0u)|((result>>31)?PC98_SF:0u);}
 if(count==1){unsigned overflow=op<4?((op&1u)?((result>>31)^((result>>30)&1u)):((result>>31)^carry)):op==4?(result>>31)^carry:op==5?value>>31:0;changed|=PC98_OF;if(overflow)status|=PC98_OF;}
 *flags=(old&~changed)|status;return result;
}
