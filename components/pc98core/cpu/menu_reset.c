/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "menu_reset.h"
#include "reset286.h"
#include <string.h>
int pc98_cpu_menu_reset(unsigned menu,Pc98Mode286 *m,Pc98Cpu386State *s){
 if(menu>2)return 0;
 memset(s,0,sizeof(*s));pc98_cpu_reset286(m,&s->low);
 s->low.model=menu?PC98_CPU_286:PC98_CPU_V30;
 s->low.base[PC98_CS]=0xf0000;m->segments.cache[PC98_CS].base=0xf0000;
 pc98_cpu_extra_segments_reset386(s);return 1;
}
