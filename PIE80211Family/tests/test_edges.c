/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/kpi_mbuf.h>
#include <pie80211/family_xnu.h>
#include "chain.h"
#include "host.h"

static unsigned char pattern[PIE80211_MAXEXT + 64];
static unsigned char out[PIE80211_MAXEXT + 64];

static int
none_left(void)
{
	return host_live() == 0 && fake_live() == 0;
}

static void
test_from_xnu(void)
{
	struct pie80211_mbuf *m;
	mbuf_t xm;

	xm = fake_chain(pattern, 1000, 100, 1);
	m = pie80211_m_from_xnu(xm, 2);
	CHECK(m != NULL && pie80211_m_pktlen(m) == 1000);
	CHECK(pie80211_m_tobuf(m, out, sizeof(out)) == 1000 && memcmp(out, pattern, 1000) == 0);
	pie80211_m_freem(m);
	mbuf_freem(xm);

	CHECK(pie80211_m_from_xnu(NULL, 0) == NULL);
	CHECK(mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_DATA, &xm) == 0);
	CHECK(pie80211_m_from_xnu(xm, 0) == NULL);
	mbuf_freem(xm);

	xm = fake_chain(pattern, 100, 100, 0);
	CHECK(pie80211_m_from_xnu(xm, 0) == NULL);
	mbuf_freem(xm);

	xm = fake_chain(pattern, PIE80211_MAXEXT + 1, 4096, 1);
	CHECK(pie80211_m_from_xnu(xm, 0) == NULL);
	mbuf_freem(xm);
	xm = fake_chain(pattern, PIE80211_MAXEXT - 1, 4096, 1);
	CHECK(pie80211_m_from_xnu(xm, 2) == NULL);
	m = pie80211_m_from_xnu(xm, 1);
	CHECK(m != NULL && pie80211_m_pktlen(m) == PIE80211_MAXEXT - 1);
	pie80211_m_freem(m);
	CHECK(pie80211_m_from_xnu(xm, PIE80211_MLEN + 1) == NULL);
	mbuf_freem(xm);

	xm = fake_chain(pattern, 600, 100, 1);
	host_fail_after(0);
	CHECK(pie80211_m_from_xnu(xm, 0) == NULL);
	host_fail_after(1);
	CHECK(pie80211_m_from_xnu(xm, 0) == NULL);
	fake_set_pktlen(xm, 700);
	CHECK(pie80211_m_from_xnu(xm, 0) == NULL);
	mbuf_freem(xm);
	CHECK(none_left());
}

static void
test_to_xnu(void)
{
	static const int sizes[] = { 100, 2000, 50 };
	struct pie80211_mbuf *m;
	mbuf_t xm;

	m = test_chain(pattern, sizes, 3);
	CHECK(test_segments(m) == 3);
	CHECK(pie80211_m_to_xnu(m, &xm) == 0 && xm != NULL);
	CHECK(xm != NULL && mbuf_pkthdr_len(xm) == 2150);
	CHECK(xm != NULL && mbuf_copydata(xm, 0, 2150, out) == 0 && memcmp(out, pattern, 2150) == 0);
	mbuf_freem(xm);

	m = test_chain(pattern, sizes, 3);
	fake_fail_after(0);
	CHECK(pie80211_m_to_xnu(m, &xm) == ENOMEM && xm == NULL);
	m = test_chain(pattern, sizes, 3);
	fake_fail_after(3);
	CHECK(pie80211_m_to_xnu(m, &xm) == ENOBUFS && xm == NULL);

	m = test_chain(pattern, sizes, 0);
	CHECK(m == NULL);
	m = pie80211_m_frombuf(pattern, 10, 0);
	CHECK(pie80211_m_to_xnu(m, &xm) == 0);
	mbuf_freem(xm);
	CHECK(none_left());
}

int
main(void)
{
	size_t i;

	for (i = 0; i < sizeof(pattern); i++)
		pattern[i] = (unsigned char)(i * 13 + (i >> 9));
	test_from_xnu();
	test_to_xnu();
	return host_report("test_edges");
}
