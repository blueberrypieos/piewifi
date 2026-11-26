/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include "host.h"

_Static_assert(sizeof(struct pkthdr) == 24, "packet header ABI");
_Static_assert(offsetof(struct pkthdr, len) == 8, "packet length ABI");
_Static_assert(offsetof(struct pkthdr, ph_rxflags) == 12, "packet flags ABI");
_Static_assert(offsetof(struct pkthdr, ph_tstamp) == 16, "packet timestamp ABI");
_Static_assert(offsetof(struct pkthdr, ph_freq) == 20, "packet frequency ABI");
_Static_assert(offsetof(struct pkthdr, ph_rssi) == 22, "packet RSSI ABI");
_Static_assert(offsetof(struct mbuf, m_pkthdr) == 40, "mbuf header ABI");
_Static_assert(offsetof(struct mbuf, m_ext) == 64, "mbuf external storage ABI");
_Static_assert(offsetof(struct mbuf, m_dat) == 80, "mbuf inline storage ABI");

static uint8_t pattern[PIE80211_MAXEXT + 64];
static uint8_t scratch[PIE80211_MAXEXT + 64];

static struct mbuf *
chain(const int *sizes, int n)
{
	struct mbuf *head = NULL, *m, **link = &head;
	int i, off = 0;

	for (i = 0; i < n; i++) {
		m = i == 0 ? m_gethdr(M_DONTWAIT, MT_DATA) : m_get(M_DONTWAIT, MT_DATA);
		if (m == NULL)
			abort();
		if (sizes[i] > MLEN)
			MCLGET(m, M_DONTWAIT);
		memcpy(mtod(m, uint8_t *), pattern + off, (size_t)sizes[i]);
		m->m_len = (u_int)sizes[i];
		off += sizes[i];
		*link = m;
		link = &m->m_next;
	}
	head->m_pkthdr.len = off;
	return head;
}

static int
chain_len(struct mbuf *m)
{
	int n = 0;

	for (; m != NULL; m = m->m_next)
		n += (int)m->m_len;
	return n;
}

static int
chain_is(struct mbuf *m, int from, int len)
{
	if (chain_len(m) != len || m->m_pkthdr.len != len)
		return 0;
	m_copydata(m, 0, len, scratch);
	return memcmp(scratch, pattern + from, (size_t)len) == 0;
}

static int
count_cb(void *arg, const void *data, size_t len)
{
	*(size_t *)arg += len;
	return 0;
}

static void
test_alloc(void)
{
	static const size_t lens[] = { 1, 223, 224, 225, 2047, 2048, 16000, 16384 };
	static const size_t leads[] = { 0, 2, 224 };
	struct mbuf *m;
	size_t i, j, seen;
	void *data;

	for (i = 0; i < nitems(lens); i++) {
		for (j = 0; j < nitems(leads); j++) {
			m = pie80211_m_frombuf(pattern, lens[i], leads[j]);
			if (lens[i] + leads[j] > PIE80211_MAXEXT) {
				CHECK(m == NULL);
				continue;
			}
			CHECK(m != NULL);
			if (m == NULL)
				continue;
			CHECK(pie80211_m_pktlen(m) == lens[i]);
			CHECK(pie80211_m_leadingspace(m) == (int)leads[j]);
			memset(scratch, 0, sizeof(scratch));
			CHECK(pie80211_m_tobuf(m, scratch, lens[i]) == (int)lens[i]);
			CHECK(memcmp(scratch, pattern, lens[i]) == 0);
			CHECK(pie80211_m_tobuf(m, scratch, lens[i] - 1) == -1);
			seen = 0;
			CHECK(pie80211_m_foreach(m, count_cb, &seen) == 0 && seen == lens[i]);
			m_freem(m);
		}
	}
	CHECK(pie80211_m_alloc(PIE80211_MAXEXT + 1, 0, &data) == NULL);
	CHECK(pie80211_m_alloc(16, PIE80211_MLEN + 1, &data) == NULL);
	host_fail_after(0);
	CHECK(pie80211_m_frombuf(pattern, 10, 0) == NULL);
	host_fail_after(1);
	CHECK(pie80211_m_frombuf(pattern, 3000, 0) == NULL);
	CHECK(host_live() == 0);
}

static void
test_clget(void)
{
	struct mbuf *m;

	CHECK(m_clget(NULL, M_DONTWAIT, 0) == NULL);
	CHECK(m_clget(NULL, M_DONTWAIT, PIE80211_MAXEXT + 1) == NULL);
	m = m_clget(NULL, M_DONTWAIT, MCLBYTES);
	CHECK(m != NULL && ISSET(m->m_flags, M_EXT | M_PKTHDR) && m->m_ext.ext_size == MCLBYTES);
	CHECK(m_trailingspace(m) == MCLBYTES);
	m_freem(m);
	host_fail_after(1);
	CHECK(m_clget(NULL, M_DONTWAIT, MCLBYTES) == NULL);
	m = m_get(M_DONTWAIT, MT_DATA);
	host_fail_after(0);
	MCLGET(m, M_DONTWAIT);
	CHECK(!ISSET(m->m_flags, M_EXT));
	m_freem(m);
	CHECK(host_live() == 0);
}

static void
test_adj(void)
{
	static const int sizes[] = { 10, 20, 30 };
	struct mbuf *m;

	m = chain(sizes, 3);
	m_adj(m, 0);
	CHECK(chain_is(m, 0, 60));
	m_adj(m, 5);
	CHECK(chain_is(m, 5, 55));
	m_adj(m, 20);
	CHECK(chain_is(m, 25, 35));
	m_adj(m, -5);
	CHECK(chain_is(m, 25, 30));
	m_adj(m, -25);
	CHECK(chain_is(m, 25, 5));
	m_adj(m, -100);
	CHECK(chain_len(m) == 0 && m->m_pkthdr.len == 0);
	m_freem(m);
	m = chain(sizes, 3);
	m_adj(m, 100);
	CHECK(chain_len(m) == 0 && m->m_pkthdr.len == 0);
	m_freem(m);
	CHECK(host_live() == 0);
}

static void
test_pullup(void)
{
	static const int small[] = { 5, 5, 5 };
	static const int big[] = { 200, 200 };
	struct mbuf *m;

	m = chain(small, 3);
	m = m_pullup(m, 12);
	CHECK(m != NULL && m->m_len >= 12 && chain_is(m, 0, 15));
	m_freem(m);
	m = chain(small, 3);
	m = m_pullup(m, 15);
	CHECK(m != NULL && m->m_len == 15 && m->m_next == NULL && chain_is(m, 0, 15));
	m_freem(m);
	m = chain(small, 3);
	CHECK(m_pullup(m, 16) == NULL);
	m = chain(big, 2);
	m = m_pullup(m, 300);
	CHECK(m != NULL && m->m_len >= 300 && chain_is(m, 0, 400));
	m_freem(m);
	m = chain(big, 2);
	host_fail_after(0);
	CHECK(m_pullup(m, 300) == NULL);
	m = chain(big, 2);
	host_fail_after(1);
	CHECK(m_pullup(m, 300) == NULL);
	m = chain(small, 3);
	CHECK(m_pullup(m, PIE80211_MAXEXT + 1) == NULL);
	CHECK(host_live() == 0);
}

static void
test_prepend(void)
{
	static const int sizes[] = { 10, 10 };
	struct mbuf *m;

	m = chain(sizes, 2);
	m_adj(m, 4);
	M_PREPEND(m, 4, M_DONTWAIT);
	CHECK(m != NULL && chain_is(m, 0, 20));
	M_PREPEND(m, 8, M_DONTWAIT);
	CHECK(m != NULL && m->m_len == 8 && ISSET(m->m_flags, M_PKTHDR) &&
	    !ISSET(m->m_next->m_flags, M_PKTHDR) && m->m_pkthdr.len == 28);
	m_freem(m);
	m = chain(sizes, 2);
	host_fail_after(0);
	M_PREPEND(m, 8, M_DONTWAIT);
	CHECK(m == NULL);
	m = chain(sizes, 2);
	M_PREPEND(m, MLEN + 1, M_DONTWAIT);
	CHECK(m == NULL);
	CHECK(host_live() == 0);
}

static void
test_split(void)
{
	static const int sizes[] = { 10, 20, 30 };
	static const int cuts[] = { 0, 5, 10, 15, 30, 59, 60 };
	struct mbuf *m, *n;
	size_t i;

	for (i = 0; i < nitems(cuts); i++) {
		m = chain(sizes, 3);
		m->m_pkthdr.ph_cookie = &m;
		n = m_split(m, cuts[i], M_DONTWAIT);
		CHECK(n != NULL);
		if (n == NULL) {
			m_freem(m);
			continue;
		}
		CHECK(chain_is(m, 0, cuts[i]));
		CHECK(chain_is(n, cuts[i], 60 - cuts[i]));
		CHECK(n->m_pkthdr.ph_cookie == &m);
		m_freem(m);
		m_freem(n);
	}
	m = chain(sizes, 3);
	CHECK(m_split(m, 61, M_DONTWAIT) == NULL && chain_is(m, 0, 60));
	host_fail_after(0);
	CHECK(m_split(m, 15, M_DONTWAIT) == NULL && chain_is(m, 0, 60));
	m_freem(m);
	CHECK(host_live() == 0);
}

static void
test_copydata_cat_dup(void)
{
	static const int a[] = { 7, 9 }, b[] = { 4 };
	struct mbuf *m, *n, *d;
	uint8_t out[32];

	m = chain(a, 2);
	memset(out, 0xee, sizeof(out));
	m_copydata(m, 10, 10, out);
	CHECK(memcmp(out, pattern + 10, 6) == 0);
	CHECK(out[6] == 0 && out[9] == 0 && out[10] == 0xee);
	n = chain(b, 1);
	n->m_flags = 0;
	m_cat(m, n);
	m->m_pkthdr.len = 20;
	CHECK(chain_len(m) == 20);
	SET(m->m_flags, M_MCAST);
	m->m_pkthdr.ph_cookie = out;
	d = m_dup_pkt(m, ETHER_ALIGN, M_DONTWAIT);
	CHECK(d != NULL && d->m_next == NULL && d->m_len == 20 && pie80211_m_leadingspace(d) == ETHER_ALIGN);
	CHECK(d != NULL && ISSET(d->m_flags, M_MCAST) && d->m_pkthdr.ph_cookie == out);
	m_copydata(d, 0, 16, out);
	CHECK(memcmp(out, pattern, 16) == 0);
	m_freem(d);
	host_fail_after(0);
	CHECK(m_dup_pkt(m, 0, M_DONTWAIT) == NULL);
	m_freem(m);
	CHECK(host_live() == 0);
}

static void
test_queues(void)
{
	struct mbuf_queue mq;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *a, *b, *c;

	mq_init(&mq, 2, IPL_NET);
	a = m_gethdr(M_DONTWAIT, MT_DATA);
	b = m_gethdr(M_DONTWAIT, MT_DATA);
	c = m_gethdr(M_DONTWAIT, MT_DATA);
	CHECK(mq_enqueue(&mq, a) == 0 && mq_enqueue(&mq, b) == 0);
	CHECK(mq_enqueue(&mq, c) == ENOBUFS && mq.mq_drops == 1 && mq_len(&mq) == 2);
	CHECK(mq_dequeue(&mq) == a && mq_dequeue(&mq) == b && mq_dequeue(&mq) == NULL);
	CHECK(mq_empty(&mq));
	ml_enqueue(&ml, a);
	ml_enqueue(&ml, b);
	CHECK(ml_dequeue(&ml) == a);
	ml_enqueue(&ml, a);
	CHECK(ml_dequeue(&ml) == b && ml_dequeue(&ml) == a && ml_dequeue(&ml) == NULL);
	ml_enqueue(&ml, a);
	ml_enqueue(&ml, b);
	CHECK(ml_purge(&ml) == 2 && ml_dequeue(&ml) == NULL);
	CHECK(host_live() == 0);
}

static void
test_align(void)
{
	struct mbuf *m = m_gethdr(M_DONTWAIT, MT_DATA);

	m_align(m, 6);
	CHECK(pie80211_m_leadingspace(m) + 6 <= MLEN && pie80211_m_leadingspace(m) % sizeof(long) == 0);
	CHECK(pie80211_m_leadingspace(m) + 6 + (int)sizeof(long) > MLEN);
	m_freem(m);
	CHECK(host_live() == 0);
}

int
main(void)
{
	size_t i;

	for (i = 0; i < sizeof(pattern); i++)
		pattern[i] = (uint8_t)(i * 7 + (i >> 8));
	test_alloc();
	test_clget();
	test_adj();
	test_pullup();
	test_prepend();
	test_split();
	test_copydata_cat_dup();
	test_queues();
	test_align();
	return host_report("test_mbuf");
}
