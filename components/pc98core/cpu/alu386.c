/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "alu386.h"
#include <assert.h>
uint32_t pc98_cpu_alu386(enum Pc98AluOp op,uint32_t left,uint32_t right,uint32_t *flags){
 assert(op>=PC98_ADD&&op<=PC98_NEG);
 uint32_t old=*flags,a=left,b=right,result,carry=0,overflow=0,auxiliary=0;
 unsigned subtract=op==PC98_SUB||op==PC98_SBB||op==PC98_CMP||op==PC98_DEC||op==PC98_NEG;
 unsigned logical=op==PC98_OR||op==PC98_AND||op==PC98_XOR;
 if(op==PC98_INC||op==PC98_DEC)b=1;
 if(op==PC98_NEG){a=0;b=left;}
 if(logical){result=op==PC98_OR?a|b:op==PC98_AND?a&b:a^b;auxiliary=old&PC98_AF;}
 else{
  unsigned input=(op==PC98_ADC||op==PC98_SBB)?old&PC98_CF:0;
  if(subtract){uint64_t subtrahend=(uint64_t)b+input;result=(uint32_t)((uint64_t)a-subtrahend);carry=(uint64_t)a<subtrahend;overflow=((a^b)&(a^result))>>31;}
  else{uint64_t sum=(uint64_t)a+b+input;result=(uint32_t)sum;carry=(uint32_t)(sum>>32);overflow=(~(a^b)&(a^result))>>31;}
  auxiliary=(a^b^result)&PC98_AF;
 }
 if(op==PC98_INC||op==PC98_DEC)carry=old&PC98_CF;
 unsigned parity=0;for(unsigned i=0;i<8;i++)parity^=(result>>i)&1u;
 *flags=(old&~(uint32_t)(PC98_CF|PC98_PF|PC98_AF|PC98_ZF|PC98_SF|PC98_OF))|carry|auxiliary|(!parity?PC98_PF:0u)|(!result?PC98_ZF:0u)|((result>>31)?PC98_SF:0u)|(overflow?PC98_OF:0u);
 return result;
}
