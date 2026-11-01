/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pc98core ROM loading.
 */
#ifndef PC98CORE_ROM_H
#define PC98CORE_ROM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load the BIOS ROM image (E8000h-FFFFFh, 0x18000 bytes) into the memory map and
 * mirror it into the ITF bank areas. Source: the file named by g_bios_file on the
 * SD card if set and loadable, otherwise the built-in PC98N image
 * (":builtin/BIOS.ROM"). The image is used as-is: nothing is patched.
 * Returns 0 on success, -1 if no ROM could be loaded. */
int pc98_rom_load(void);

#ifdef __cplusplus
}
#endif

#endif
