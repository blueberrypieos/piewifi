/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include "chain.h"

struct pie80211_mbuf *
test_chain(const void *data, const int *sizes, int n)
{
	const uint8_t *p = data;
	struct mbuf *head = NULL, *m, **link = &head;
	int i, total = 0;

	for (i = 0; i < n; i++) {
		m = i == 0 ? m_gethdr(M_DONTWAIT, MT_DATA) : m_get(M_DONTWAIT, MT_DATA);
		if (m == NULL)
			abort();
		if (sizes[i] > MLEN)
			MCLGET(m, M_DONTWAIT);
		memcpy(mtod(m, uint8_t *), p + total, (size_t)sizes[i]);
		m->m_len = (u_int)sizes[i];
		total += sizes[i];
		*link = m;
		link = &m->m_next;
	}
	if (head != NULL)
		head->m_pkthdr.len = total;
	return head;
}

int
test_segments(struct pie80211_mbuf *m)
{
	int n = 0;

	for (; m != NULL; m = m->m_next)
		n++;
	return n;
}
