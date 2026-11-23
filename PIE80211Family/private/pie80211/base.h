/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_BASE_H
#define _PIE80211_BASE_H

#ifdef PIE80211_HOST
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/time.h>
void panic(const char *, ...) __attribute__((__noreturn__, __format__(__printf__, 1, 2)));
static inline u_int
min(u_int x, u_int y)
{
	return y < x ? y : x;
}
#else
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <kern/debug.h>
#include <libkern/libkern.h>
#define abs(x)		__builtin_abs(x)
#endif

#include <pie80211/names.h>

#ifndef __packed
#define __packed	__attribute__((__packed__))
#endif
#ifndef __unused
#define __unused	__attribute__((__unused__))
#endif
#ifndef __aligned
#define __aligned(n)	__attribute__((__aligned__(n)))
#endif
#ifndef __predict_true
#define __predict_true(e)	__builtin_expect(((e) != 0), 1)
#endif
#ifndef __predict_false
#define __predict_false(e)	__builtin_expect(((e) != 0), 0)
#endif
#define __bounded__(...)
#ifndef CTASSERT
#define CTASSERT(e)	_Static_assert(e, #e)
#endif
#ifndef ALIGNED_POINTER
#define ALIGNED_POINTER(p, t)	((((uintptr_t)(p)) & (sizeof(t) - 1)) == 0)
#endif

#ifndef MIN
#define MIN(x, y)	((y) < (x) ? (y) : (x))
#endif
#ifndef MAX
#define MAX(x, y)	((y) > (x) ? (y) : (x))
#endif
#ifndef setbit
#define setbit(v, i)	(((u_int8_t *)(v))[(i) >> 3] |= (u_int8_t)(1U << ((i) & 7)))
#define clrbit(v, i)	(((u_int8_t *)(v))[(i) >> 3] &= (u_int8_t)~(1U << ((i) & 7)))
#define isset(v, i)	((((const u_int8_t *)(v))[(i) >> 3] & (1U << ((i) & 7))) != 0)
#define isclr(v, i)	((((const u_int8_t *)(v))[(i) >> 3] & (1U << ((i) & 7))) == 0)
#endif

#ifndef nitems
#define nitems(v)	(sizeof(v) / sizeof(*(v)))
#endif
#ifndef SET
#define SET(v, bits)	((v) |= (bits))
#endif
#ifndef ISSET
#define ISSET(v, bits)	(((v) & (bits)) != 0)
#endif
#ifndef CLR
#define CLR(v, bits)	((v) &= ~(bits))
#endif

#undef htole16
#undef htole32
#undef htole64
#undef htobe16
#undef htobe32
#undef htobe64
#define htole16(x)	((uint16_t)(x))
#define htole32(x)	((uint32_t)(x))
#define htole64(x)	((uint64_t)(x))
#define letoh16(x)	((uint16_t)(x))
#define letoh32(x)	((uint32_t)(x))
#define letoh64(x)	((uint64_t)(x))
#define swap16(x)	__builtin_bswap16(x)
#define swap32(x)	__builtin_bswap32(x)
#define swap64(x)	__builtin_bswap64(x)
#define htobe16(x)	swap16((uint16_t)(x))
#define htobe32(x)	swap32((uint32_t)(x))
#define htobe64(x)	swap64((uint64_t)(x))
#define betoh16(x)	swap16((uint16_t)(x))
#define betoh32(x)	swap32((uint32_t)(x))
#define betoh64(x)	swap64((uint64_t)(x))
#ifndef htons
#define htons(x)	htobe16(x)
#define ntohs(x)	betoh16(x)
#define htonl(x)	htobe32(x)
#define ntohl(x)	betoh32(x)
#endif

#include <pie80211/platform.h>

#undef KASSERT
#define KASSERT(e)	((e) ? (void)0 : pie80211_kassert(#e, __FILE__, __LINE__))
#define DIAGNOSTIC_ONLY(x)

#define explicit_bzero(p, n)		pie80211_explicit_bzero(p, n)
#define timingsafe_bcmp(a, b, n)	pie80211_timingsafe_bcmp(a, b, n)
#define ether_sprintf(a)		pie80211_ether_sprintf(a)
void pie80211_explicit_bzero(void *p, size_t n);
int pie80211_timingsafe_bcmp(const void *a, const void *b, size_t n);
char *pie80211_ether_sprintf(const uint8_t *a);

#endif
