/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "boundary.h"
Pc98BoundaryResult pc98_cpu_boundary(unsigned menu,Pc98Mode286 *m,Pc98Cpu386State *s,const Pc98ExecuteBus *io,const Pc98BoundaryRequest *request){
 if(io->instruction_completed)*io->instruction_completed=0;
 Pc98BoundaryResult result={.action=PC98_BOUNDARY_STEP,.step=PC98_STEP_UNSUPPORTED,.interrupt=PC98_INT_BLOCKED,.kind=PC98_INT_SOFTWARE};
 if(request){
  for(unsigned priority=0;priority<2;priority++){
   enum Pc98InterruptKind kind=priority?PC98_INT_IRQ:PC98_INT_NMI;
   if(priority?!request->irq:!request->nmi)continue;
   unsigned vector=priority?request->irq_vector:2u;
   enum Pc98InterruptResult delivered=pc98_cpu_menu_interrupt(menu,m,s,io,vector,kind);
   if(delivered!=PC98_INT_BLOCKED){result.action=PC98_BOUNDARY_EXTERNAL;result.interrupt=delivered;result.kind=kind;return result;}
  }
 }
 result.step=pc98_cpu_menu_step(menu,m,s,io);return result;
}
