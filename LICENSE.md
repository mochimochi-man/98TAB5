# 98TAB5 — Licensing and Third-Party Notices

98TAB5 is a PC-9801 emulator for the **M5Stack Tab5** (ESP32-P4).

- Author: **mochimochi-man**
- Contact: **X [@MochimochimanEX](https://x.com/MochimochimanEX)**

This document is the authoritative statement of what this distribution
contains and under what terms it may be redistributed. `LICENSE.jp.md` is a
convenience translation; where the two differ, this document governs.

Verbatim copies of every licence referred to below are in [`licenses/`](licenses/).

---

## 1. Summary

| | |
|---|---|
| Code written for this project | **MIT** — see §2 |
| Everything else | its own licence, all **permissive** — see §4 |
| Copyleft (GPL/LGPL) content | **none**, in the image or in this archive — see §5 |
| NEC PC-9801 ROM, BIOS or font data | **none** — see §3 |

The firmware image is a combined work. The author's own code is MIT; every
other part keeps the licence of its own author. Redistributing the image or
this archive means satisfying all of them at once — which is not onerous, as
all are permissive, but it does mean **this project as a whole is not simply
"MIT"**. The practical obligations are listed in §6.

---

## 2. Code written for this project

> MIT License
>
> Copyright (c) 2026 mochimochi-man
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

**What this covers:** everything under `main/` and `rom_src/`, the top-level
`CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv` and the build scripts —
that is, the Tab5 port itself: the MIPI-DSI panel bring-up, the PPA blitter, the
menu, the touch-as-mouse driver, the USB mass-storage modes, the BLE HID host,
the screenshot writer and the RTC support.

It also covers `components/pc98core/`, the whole emulated machine: the CPU
(`cpu/`), the devices (`dev/`), video (`video/`), sound (`sound/`) and the disk
media (`media/`).

**What this does not cover — two files inside `main/`:**

- `main/st7123_init_data.h`
- `main/ili9881c_init_data.h`

These are **verbatim copies** of panel initialisation data from the M5Stack Tab5
BSP and remain under **Apache-2.0** (§4.4). Each carries its notice in its own
header.

---

## 3. What this project deliberately does **not** contain

- **No NEC PC-9801 BIOS, ITF or CGROM data**, in any form.
  - The BIOS built into the image (`main/rom/PC98N.ROM`) is an independent
    implementation, built from `rom_src/pc98_bios_native/pc98n.asm` in this
    archive, which reproduces it byte for byte. It contains no NEC code, and
    none from any other vendor's BIOS either: compared against a genuine EPSON
    BIOS.ROM, no run of 16 bytes or longer is common to the two images and it
    carries no vendor copyright notice but its own.
  - The font built into the image (`main/rom/FONT_ESP.ROM`) is generated from
    the public-domain Shinonome bitmap fonts by `rom_src/mkfont_esp.py`
    (§4.5). It is not a dump of any real machine's character generator.
  - Both are built into the firmware so that the emulator starts on a blank SD
    card. The menu can still be pointed at a real dumped ROM on the card, which
    then takes precedence; supplying such a dump is the user's own affair.
- **No disk images, games or other software** for the emulated machine.

---

## 4. Third-party components

### 4.1 ESP-IDF v5.5 — Espressif Systems

**Apache-2.0.** ESP-IDF ships no `NOTICE` file. A verbatim copy of the licence
is in `licenses/Apache-2.0.txt`, which is what §4(a) of that licence requires a
redistributor to pass on.

Parts of ESP-IDF carry their own terms; those that reach this image are:

- **FatFs** — Copyright (C) 2022, ChaN. Its own one-condition permissive
  licence: redistributions of source must retain the copyright notice.
  See `licenses/FatFs.txt`.
- **TinyUSB** — **MIT**, Copyright (c) Ha Thach (tinyusb.org).
- **miniz**, in the ESP32-P4 boot ROM and used by the screenshot writer.
  Upstream miniz is **MIT**: Copyright 2013-2014 RAD Game Tools and Valve
  Software; Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC.
  ESP-IDF's `miniz.h`, which declares it, is Apache-2.0 (Espressif).
- **Bluedroid** — Apache-2.0, as distributed with ESP-IDF.
- **Newlib** and the RISC-V toolchain runtime — BSD-family licences; see the
  ESP-IDF and toolchain trees.

### 4.2 Espressif managed components

Pulled from the ESP Component Registry at build time; the exact versions are
pinned in `dependencies.lock`. They are **not** included in this archive — a
build downloads them.

**Apache-2.0**: `bmi270`, `cmake_utilities`, `eppp_link`, `esp_cam_sensor`,
`esp_codec_dev`, `esp_h264`, `esp_hosted`, `esp_io_expander`,
`esp_io_expander_pi4ioe5v6408`, `esp_lcd_ili9881c`, `esp_lcd_st7121`,
`esp_lcd_st7123`, `esp_lcd_touch`, `esp_lcd_touch_gt911`,
`esp_lcd_touch_st7123`, `esp_lvgl_port`, `esp_sccb_intf`,
`esp_serial_slave_link`, `esp_wifi_remote`, `i2c_bus`, `m5stack_tab5`,
`sensor_hub`, `usb`, `usb_host_hid`, `usb_host_uvc`,
`wifi_remote_over_eppp`.

**MIT**: `tinyusb`, `esp_ipa`, `esp_video`.

### 4.3 LVGL

**MIT**, Copyright (c) 2025 LVGL Kft. Reached through the Tab5 BSP rather than
used directly by this project. Its bundled VGLite kernel driver (Vivante
Corporation, 2014-2022) is dual-licensed MIT/GPL-2.0 and is taken here under
**MIT**.

### 4.4 M5Stack Tab5 BSP — Espressif Systems

**Apache-2.0.** In addition to being linked, two files are **copied verbatim**
into this project and must keep their notices:

| File here | Copied from |
|---|---|
| `main/st7123_init_data.h` | `espressif/m5stack_tab5` → `priv_include/disp_init_data_1.h` |
| `main/ili9881c_init_data.h` | `espressif/m5stack_tab5` → `priv_include/disp_init_data.h` |

### 4.5 Shinonome bitmap fonts (東雲フォント) — public domain

Used by `rom_src/mkfont_esp.py` to generate the built-in `FONT_ESP.ROM`.

- Author: Yasuyuki Furukawa; 2001-2004 /efont/ Project
- Upstream: <http://openlab.ring.gr.jp/efont/shinonome/>
- Licence: **public domain.** The archive states that all font data,
  documentation and scripts are placed in the public domain, and — because
  Japanese law does not permit an author to abandon copyright — that the
  authors listed in its `AUTHORS` declare that they will not exercise their
  rights, which the project treats as equivalent. Free modification, format
  conversion, embedding and redistribution are permitted, entirely without
  warranty. See `licenses/Shinonome.txt`.

The Debian packaging of these fonts is GPL-2+, but that covers the packaging
scripts only. No part of it is used or distributed here.

## 5. Copyleft

Nothing under a copyleft licence is compiled into the firmware **or present in
this archive**. `grep -ril "GNU General Public"` over this archive returns nothing.

---

## 6. If you redistribute this

All of the licences involved are permissive, so the obligations are modest:

1. **Keep this file and `licenses/`** with whatever you pass on.
2. **Ship a copy of the Apache-2.0 licence** with any binary that contains
   ESP-IDF, the Tab5 BSP or the Espressif components — that is, with any build
   of this project. `licenses/Apache-2.0.txt` is there for exactly this.
3. **Keep the copyright notices** in any source you redistribute, including the
   two copied headers named in §2.
4. **State your changes** if you modify Apache-2.0 material.

You do **not** have to open your own additions, and no part of this obliges you
to publish source.

---

## 7. Trademarks

PC-9801, PC-98 and NEC are trademarks of NEC Corporation. M5Stack and Tab5 are
trademarks of M5Stack Technology Co., Ltd. ESP32, ESP32-P4 and ESP-IDF are
trademarks of Espressif Systems. Windows is a trademark of Microsoft
Corporation. They are used here only to say what this software runs on and what
it emulates. This project is not affiliated with, endorsed by, or sponsored by
any of them.

## 8. No warranty

Every licence above disclaims all warranties, and so does this project. This is
a hobby emulator that drives hardware directly; run it at your own risk.
