/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "reset286.h"
#include <string.h>
void pc98_cpu_reset286(Pc98Mode286 *m,Pc98Cpu16State *c){
 pc98_cpu_mode_init286(m);memset(c,0,sizeof(*c));
 c->model=PC98_CPU_286;c->flags=2;c->ip=0xfff0;
 c->seg[PC98_CS]=0xf000;c->base[PC98_CS]=0xff0000;
 for(unsigned s=0;s<4;s++){
  m->segments.cache[s]=(Pc98Descriptor286){.base=c->base[s],.limit=0xffff,.access=(uint8_t)(s==PC98_CS?0x9b:0x93)};
  m->segments.valid[s]=1;
 }
 m->cache_initialized=1;
}
