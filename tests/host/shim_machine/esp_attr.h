/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Host stand-in for ESP-IDF memory attributes. */
#pragma once
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
#define RTC_DATA_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
