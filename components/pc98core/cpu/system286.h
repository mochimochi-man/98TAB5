/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80286 PRM 10.2.2, ARPL, SGDT/SIDT, LGDT/LIDT, VERR/VERW.
 * Pure host helpers, not yet system-opcode dispatch or protected execution. */
#ifndef PC98_CPU_SYSTEM286_H
#define PC98_CPU_SYSTEM286_H
#include "descriptor286.h"
/* Load/store the six-byte 286 pseudo-descriptor. Reserved final store byte
 * is deterministically FF; Intel documents it as undefined on 286. */
Pc98Table286 pc98_cpu_table_load286(const uint8_t bytes[6]);
void pc98_cpu_table_store286(const Pc98Table286 *,uint8_t bytes[6]);
/* Only low four MSW controls change. PE is sticky until reset; upper reserved
 * state is retained. Privilege and mode checks belong to instruction caller. */
uint16_t pc98_cpu_lmsw286(uint16_t current,uint16_t source);
uint16_t pc98_cpu_arpl286(uint16_t destination,uint16_t source,uint16_t *flags);
/* Nonfaulting VERR/VERW descriptor check after table lookup. Presence is not
 * tested. Failed lookup/null selector returns false; caller updates ZF only. */
int pc98_cpu_verify_data286(const Pc98Descriptor286 *,uint16_t selector,unsigned cpl,int write);
/* Nonfaulting LAR/LSL query; failure leaves the result unchanged. */
int pc98_cpu_query_descriptor286(const Pc98Descriptor286 *,uint16_t selector,unsigned cpl,int limit,uint16_t *result);
#endif
