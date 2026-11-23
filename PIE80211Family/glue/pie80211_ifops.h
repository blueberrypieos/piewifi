/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_IFOPS_H
#define _PIE80211_IFOPS_H

struct pie80211_ifnet;
struct pie80211_mbuf_list;

struct pie80211_ifops {
	void (*input)(struct pie80211_ifnet *, struct pie80211_mbuf_list *);
	void (*link_state)(struct pie80211_ifnet *);
};

#endif
