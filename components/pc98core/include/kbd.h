/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * kbd.h - the keyboard: the i8251 interface at 41h / 43h (dev/kbd.c) and the keyboard unit at the other end of
 * the serial line (dev/keyunit.c). */
#ifndef PC98_KBD_H
#define PC98_KBD_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pc98Settings;

/* dev/kbd.c */
void	kbd_reset(const struct Pc98Settings *cfg);
void	kbd_bind(void);
void	kbd_changeclock(void);		/* after g_mach.cpu_clock changed */
void	kbd_put_key(REG8 code);		/* a key byte from the unit */
void	kbd_put_reply(REG8 data);	/* an answer to a command from the unit */
void	kbd_drop_replies(void);

/* dev/keyunit.c; key codes are the PC-98 key numbers 00h-7Fh */
void	keyunit_init(void);
void	keyunit_key(UINT code, int down);	/* a key of the host keyboard */
void	keyunit_release_all(void);
void	keyunit_command(REG8 data);		/* a byte the CPU sent to the unit */
void	keyunit_reset_signal(void);		/* the interface reset the unit */
void	keyunit_tick(void);			/* once per frame: auto repeat */

#ifdef __cplusplus
}
#endif

#endif
