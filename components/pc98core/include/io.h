/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * io.h - the I/O port space (io.c).
 *
 * A device is attached the way its address decoder is wired:
 *   io_sys()   an internal device of the main board: it looks at the low address byte only, at the bits in `decode`,
 *              and answers only while A10 and A11 are 0.  Low bytes that match base on the decoded bits select
 *              register 0, 1, ... in turn (the undecoded bits make mirrors).
 *   io_any()   the same, but answering whatever the high byte is.
 *   io_port()  one port, all 16 address bits decoded.
 *   io_board() a C-bus board port: the low 12 address bits decoded.
 *   io_port16() a device that is 16 bits wide on the bus (a word access is one access, not two).
 * A port nothing is attached to ignores writes and reads FFh.  A word access to an 8-bit device is two byte
 * accesses, port then port + 1 (low byte lane, high byte lane). */
#ifndef PC98_IO_H
#define PC98_IO_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*io_out_fn)(UINT port, REG8 value);
typedef REG8 (*io_in_fn)(UINT port);
typedef void (*io_out16_fn)(UINT port, REG16 value);
typedef REG16 (*io_in16_fn)(UINT port);

/* NULL entries leave the register as it was (a write-only or read-only register) */
void	io_sys(UINT base, UINT decode, const io_out_fn *out, const io_in_fn *in, UINT regs);
void	io_any(UINT base, UINT decode, const io_out_fn *out, const io_in_fn *in, UINT regs);
void	io_port(UINT port, io_out_fn out, io_in_fn in);
void	io_board(UINT port, io_out_fn out, io_in_fn in);
void	io_board_release(UINT port);
void	io_port16(UINT port, io_out16_fn out, io_in16_fn in);

void	io_clear(void);			/* nothing attached */
void	io_free(void);
void	io_set_waitclock(UINT clocks);	/* CPU clocks every access costs */

void	io_out8(UINT port, REG8 value);
REG8	io_in8(UINT port);
void	io_out16(UINT port, REG16 value);
REG16	io_in16(UINT port);
void	io_out32(UINT port, UINT32 value);
UINT32	io_in32(UINT port);

#ifdef __cplusplus
}
#endif

#endif
