/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_FAMILY_H
#define _PIE80211_FAMILY_H

#include <stddef.h>
#include <stdint.h>

#define PIE80211_MLEN		224
#define PIE80211_MAXEXT		16384

#ifdef __cplusplus
extern "C" {
#endif

struct pie80211_mbuf;
struct pie80211_com;

struct pie80211_mbuf *pie80211_m_alloc(size_t len, size_t lead, void **data);
struct pie80211_mbuf *pie80211_m_frombuf(const void *buf, size_t len, size_t lead);
size_t pie80211_m_pktlen(struct pie80211_mbuf *m);
int pie80211_m_foreach(struct pie80211_mbuf *m, int (*fn)(void *, const void *, size_t),
    void *arg);
int pie80211_m_tobuf(struct pie80211_mbuf *m, void *buf, size_t cap);
struct pie80211_mbuf *pie80211_m_freem(struct pie80211_mbuf *m);

int pie80211_ioctl(struct pie80211_com *com, uint32_t cmd, void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
