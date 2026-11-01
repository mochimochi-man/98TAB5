/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_MODE386_H
#define PC98_CPU_MODE386_H
#include "mode286.h"
#include "execute386.h"
/* Explicit extension step with the existing286 real hidden/protected cache.
 * This is not automatic menu dispatch or a full386 protected-mode model.
 * Initialize mode in place; highGPR/FLAGS remain owned by the caller. */
/* Prepared bus additionally supplies an atomic FS/GS data-selector loader
 * using286 descriptors. Returned contexts borrow the in-place mode object;
 * caller checks model/shutdown before preparation. Not full386 descriptors. */
Pc98ExecuteBus pc98_cpu_mode_bus_extension386(Pc98Mode286 *,Pc98Cpu16State *,const Pc98ExecuteBus *);

enum Pc98StepResult pc98_cpu_mode_step_extension386(Pc98Mode286 *,Pc98Cpu386State *,const Pc98ExecuteBus *);
#endif
