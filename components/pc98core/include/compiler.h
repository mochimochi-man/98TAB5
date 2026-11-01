/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * compiler.h - the basic types and helpers every pc98core / main file uses. */
#ifndef PC98_COMPILER_H
#define PC98_COMPILER_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t		SINT8;
typedef uint8_t		UINT8;
typedef int16_t		SINT16;
typedef uint16_t	UINT16;
typedef int32_t		SINT32;
typedef uint32_t	UINT32;
typedef int64_t		SINT64;
typedef uint64_t	UINT64;
typedef signed int	SINT;
typedef unsigned int	UINT;
typedef intptr_t	INTPTR;
typedef uintptr_t	UINTPTR;

/* register-width values: arguments and results that carry 8 / 16 / 32 bits */
typedef unsigned int	REG8;
typedef unsigned int	REG16;
typedef UINT32		REG32;

typedef int		BOOL;
#ifndef TRUE
#define TRUE		1
#endif
#ifndef FALSE
#define FALSE		0
#endif

/* result of an operation that can fail */
typedef int		BRESULT;
#define SUCCESS		0
#define FAILURE		1


#ifndef MAX_PATH
#define MAX_PATH	256
#endif

#define NELEMENTS(a)	((int)(sizeof(a) / sizeof((a)[0])))
#ifndef MIN
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#endif

#define ZeroMemory(p, n)	memset((p), 0, (n))
#define FillMemory(p, n, v)	memset((p), (v), (n))
#define CopyMemory(d, s, n)	memcpy((d), (s), (n))
#define MoveMemory(d, s, n)	memmove((d), (s), (n))

/* little-endian loads and stores at any alignment */
#define LOADINTELWORD(p)	((UINT16)(((const UINT8 *)(p))[0] | (((const UINT8 *)(p))[1] << 8)))
#define LOADINTELDWORD(p)	((UINT32)LOADINTELWORD(p) | ((UINT32)LOADINTELWORD((const UINT8 *)(p) + 2) << 16))
#define STOREINTELWORD(p, v)	do { UINT8 *q_ = (UINT8 *)(p); UINT v_ = (UINT)(v); q_[0] = (UINT8)v_; q_[1] = (UINT8)(v_ >> 8); } while (0)
#define STOREINTELDWORD(p, v)	do { UINT8 *q_ = (UINT8 *)(p); UINT32 v_ = (UINT32)(v); q_[0] = (UINT8)v_; q_[1] = (UINT8)(v_ >> 8); \
					q_[2] = (UINT8)(v_ >> 16); q_[3] = (UINT8)(v_ >> 24); } while (0)

/* the low n bits */
#define LOW12(a)	((a) & 0x0fff)
#define LOW15(a)	((a) & 0x7fff)
#define LOW16(a)	((a) & 0xffff)

#define CPUCALL
#define INLINE		inline

#ifdef __cplusplus
}
#endif

#endif
