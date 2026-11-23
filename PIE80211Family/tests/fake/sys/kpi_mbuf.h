/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _FAKE_KPI_MBUF_H
#define _FAKE_KPI_MBUF_H

#include <stddef.h>

typedef int errno_t;
typedef struct fake_mbuf *mbuf_t;
typedef enum { MBUF_WAITOK, MBUF_DONTWAIT } mbuf_how_t;
typedef enum { MBUF_TYPE_DATA = 1 } mbuf_type_t;
typedef enum { MBUF_PKTHDR = 2 } mbuf_flags_t;

mbuf_flags_t mbuf_flags(mbuf_t m);
size_t mbuf_pkthdr_len(mbuf_t m);
errno_t mbuf_copydata(mbuf_t m, size_t off, size_t len, void *out);
errno_t mbuf_gethdr(mbuf_how_t how, mbuf_type_t type, mbuf_t *mp);
errno_t mbuf_copyback(mbuf_t m, size_t off, size_t len, const void *data, mbuf_how_t how);
void mbuf_freem(mbuf_t m);

mbuf_t fake_chain(const void *data, size_t len, size_t seg, int pkthdr);
void fake_set_pktlen(mbuf_t m, size_t len);
long fake_live(void);
void fake_fail_after(long n);

#endif
