/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * fast16 mini loop in RISC-V assembly (fast16_mini_rv.S): byte offsets inside Pc98Adapter used by the
 * assembly handlers. fast16.c checks every value against offsetof() at compile time. */
#ifndef PC98_FAST16_RV_H
#define PC98_FAST16_RV_H
#define F16A_R        156     /* s.low.r[8]   AX CX DX BX SP BP SI DI */
#define F16A_SEG      172     /* s.low.seg[4] ES CS SS DS */
#define F16A_FLAGS    182     /* s.low.flags */
#define F16A_BASE     184     /* s.low.base[4] */
#define F16A_IRQINH   206     /* s.low.irq_inhibit */
#define F16A_CACHE    32      /* mode.segments.cache[4], 8 bytes each: base(u32) limit(u16) access(u8) */
#define F16A_VALID    64      /* mode.segments.valid[4] (u8) */
#define F16A_MENU     592
#define F16A_ROM      656
#define F16A_ITF      660
#define F16A_SLICEP   688
#define F16A_IRQPROBE 708
#define F16A_IRQEND   712
#define F16A_MCKN     716     /* mck_n[256] */
#define F16A_MCKM     972     /* mck_m[512] */
#define F16A_MCKMAIN  1484    /* mck_main (pointer) */
#define F16A_MCKT     1488    /* mck_t (pointer) */
#define F16A_MOPCB    1500
#define F16A_MCKSHIFT 1504    /* mck_shift (pointer) */
#define F16A_MOREM    1508
#define F16A_MOOFF    1512
#define F16A_MOFL     1516
#define F16A_MONRET   1520
#define F16A_MOIP     1524
#define F16A_MOEV     1528
#define F16A_MOOV     1532
#define F16A_MOROM    1536
#endif
