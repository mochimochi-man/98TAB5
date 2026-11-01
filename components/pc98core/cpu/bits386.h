/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_BITS386_H
#define PC98_CPU_BITS386_H
#include <stdint.h>
/* Pure 16/32-bit primitives, not opcode dispatch or a complete 386 model.
 * Intel 80386 PRM (1986) 3.4.3 and 17-27..35, 17-155..158.
 * Undefined flags are preserved deterministically, never silicon claims. */
enum Pc98Bit386 {PC98_BIT_TEST386,PC98_BIT_SET386,PC98_BIT_RESET386,PC98_BIT_COMPLEMENT386};
uint32_t pc98_cpu_bit386(enum Pc98Bit386,unsigned bits,uint32_t value,unsigned index,uint32_t *flags);
/* Source zero sets ZF and leaves the undefined destination unchanged. */
int pc98_cpu_scan386(unsigned reverse,unsigned bits,uint32_t source,uint32_t *destination,uint32_t *flags);
/* Count is masked to five bits. The 1986 PRM defines count >= operand width
 * as undefined; return 0 without changes for these 16-bit counts. Count zero
 * returns 1 without changes. OF/AF remain undefined even for count one in
 * this manual; retain them instead of importing later-generation rules. */
int pc98_cpu_double_shift386(unsigned right,unsigned bits,uint32_t destination,uint32_t source,unsigned count,uint32_t *flags,uint32_t *result);
/* Signed register bit-string index uses floor division; immediate index
 * stays within the addressed word/dword. Return 0 without output changes
 * when adjusted address would cross the address-size wrap boundary: the
 * physical generation-specific wrap contract remains explicitly unsupported. */
int pc98_cpu_bit_address386(unsigned bits,unsigned address_bits,uint32_t base,uint32_t index,unsigned immediate,uint32_t *address,unsigned *bit);
#endif
