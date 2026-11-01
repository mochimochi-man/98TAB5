/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "menu_shut.h"
#include <string.h>
/* A CPU reset is a processor RESET: the cold reset state (EFLAGS 2, FS/GS selector 0 base 0 limit FFFFh, Intel386
 * PRM 10.1), except that the upper halves of the general registers, which the PRM leaves undefined, keep their
 * values. */
int pc98_cpu_menu_shut(unsigned menu,Pc98Mode286 *m,Pc98Cpu386State *s){
 if(menu>2)return 0;
 uint16_t high[8];memcpy(high,s->high,sizeof(high));
 (void)pc98_cpu_menu_reset(menu,m,s);
 memcpy(s->high,high,sizeof(high));
 return 1;
}
