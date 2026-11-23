/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _CHAIN_H
#define _CHAIN_H

struct pie80211_mbuf;

struct pie80211_mbuf *test_chain(const void *data, const int *sizes, int n);
int test_segments(struct pie80211_mbuf *m);

#endif
