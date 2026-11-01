/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pcstr.c - see pcstr.h. */
#include "pcstr.h"

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

int pcs_eqi(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

int pcs_u8len(const char *p)
{
    const unsigned c = (unsigned char)*p;
    int n, i;

    if (c == 0) return 0;
    n = c < 0xc0u ? 1 : c < 0xe0u ? 2 : c < 0xf0u ? 3 : 4;
    for (i = 1; i < n; i++) {
        if (((unsigned char)p[i] & 0xc0u) != 0x80u) return 1;   /* broken sequence: one byte at a time */
    }
    return n;
}

void pcs_copy(char *dst, const char *src, size_t size)
{
    size_t used = 0;
    int n;

    if (size == 0) return;
    while ((n = pcs_u8len(src)) != 0 && used + (size_t)n < size) {
        while (n--) dst[used++] = *src++;
    }
    dst[used] = '\0';
}
