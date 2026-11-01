/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
static inline void *heap_caps_malloc(size_t n,int caps){(void)caps;return malloc(n);}
static inline void *heap_caps_calloc(size_t c,size_t n,int caps){(void)caps;return calloc(c,n);}
static inline void heap_caps_free(void *p){free(p);}
