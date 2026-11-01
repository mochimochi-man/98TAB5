/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80286 PRM sections 6.3/7.4, MOV checks (8-64), expand-down 11.2.2.
 * Pure host-only foundation, not yet connected to protected execution. */
#ifndef PC98_CPU_DESCRIPTOR286_H
#define PC98_CPU_DESCRIPTOR286_H
#include <stdint.h>
#include "decode.h"
typedef struct {uint32_t base;uint16_t limit;uint8_t access;} Pc98Descriptor286;
Pc98Descriptor286 pc98_cpu_descriptor286(const uint8_t bytes[8]);
typedef struct {uint32_t base;uint16_t limit;} Pc98Table286;
typedef struct {Pc98Table286 gdt,ldt;uint8_t ldt_valid;} Pc98DescriptorTables286;
/* Returns 0 on success, 13 (#GP) for invalid selector/table bounds. No bus
 * reads occur on failure and output is unchanged. Null DS/ES selectors
 * must be handled by the segment loader before calling this lookup. */
unsigned pc98_cpu_lookup286(const Pc98FetchBus *,const Pc98DescriptorTables286 *,
                           uint16_t selector,Pc98Descriptor286 *out,uint16_t *error);
/* Return exception vector 0 (success), 11 (#NP), 12 (#SS), 13 (#GP).
 * Null DS/ES selectors succeed without consulting descriptor. The caller
 * clears its cache valid bit. SS never accepts a null selector.
 * Missing nonnull descriptors represent a table lookup failure. Error code
 * is zero for null selectors, otherwise selector with RPL bits removed. */
unsigned pc98_cpu_load_data286(const Pc98Descriptor286 *,uint16_t selector,
                              unsigned cpl,int stack,uint16_t *error);
/* Direct code-segment JMP checks, after selector/table lookup. Gate and
 * task descriptors must be routed separately by the future instruction core.
 * Null/type/privilege -> #GP(selector), absent -> #NP(selector), IP -> #GP(0). */
unsigned pc98_cpu_check_code286(const Pc98Descriptor286 *,uint16_t selector,
                               unsigned cpl,uint16_t offset,uint16_t *error);
/* Bounds only: access rights/presence and cache validity are separate checks.
 * size must be nonzero; words never wrap the 16-bit segment boundary. */
int pc98_cpu_range286(const Pc98Descriptor286 *,uint16_t offset,uint32_t size);
#endif
