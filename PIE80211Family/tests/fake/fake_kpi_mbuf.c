/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/kpi_mbuf.h>

#define FAKE_SEG	100

struct fake_mbuf {
	struct fake_mbuf *next;
	size_t len;
	size_t cap;
	int flags;
	size_t pktlen;
	unsigned char data[];
};

static long live;
static long fail_in = -1;

long
fake_live(void)
{
	return live;
}

void
fake_fail_after(long n)
{
	fail_in = n;
}

static mbuf_t
seg_new(size_t cap)
{
	mbuf_t m;

	if (fail_in == 0) {
		fail_in = -1;
		return NULL;
	}
	if (fail_in > 0)
		fail_in--;
	m = calloc(1, sizeof(*m) + cap);
	if (m != NULL) {
		m->cap = cap;
		live++;
	}
	return m;
}

mbuf_flags_t
mbuf_flags(mbuf_t m)
{
	return (mbuf_flags_t)m->flags;
}

size_t
mbuf_pkthdr_len(mbuf_t m)
{
	return m->pktlen;
}

errno_t
mbuf_copydata(mbuf_t m, size_t off, size_t len, void *out)
{
	unsigned char *p = out;
	size_t take;

	for (; m != NULL && len > 0; m = m->next) {
		if (off >= m->len) {
			off -= m->len;
			continue;
		}
		take = m->len - off < len ? m->len - off : len;
		memcpy(p, m->data + off, take);
		p += take;
		len -= take;
		off = 0;
	}
	return len == 0 ? 0 : EINVAL;
}

errno_t
mbuf_gethdr(mbuf_how_t how, mbuf_type_t type, mbuf_t *mp)
{
	*mp = seg_new(FAKE_SEG);
	if (*mp == NULL)
		return ENOMEM;
	(*mp)->flags = MBUF_PKTHDR;
	return 0;
}

errno_t
mbuf_copyback(mbuf_t m, size_t off, size_t len, const void *data, mbuf_how_t how)
{
	const unsigned char *p = data;
	mbuf_t head = m;
	size_t end = off + len, take;

	if (m == NULL || len == 0 || data == NULL)
		return EINVAL;
	for (;;) {
		if (off < m->cap) {
			take = m->cap - off < len ? m->cap - off : len;
			memcpy(m->data + off, p, take);
			if (m->len < off + take)
				m->len = off + take;
			p += take;
			len -= take;
			off = 0;
		} else {
			if (m->len < m->cap && m->next == NULL)
				m->len = m->cap;
			off -= m->cap;
		}
		if (len == 0)
			break;
		if (m->next == NULL && (m->next = seg_new(FAKE_SEG)) == NULL)
			return ENOBUFS;
		m = m->next;
	}
	if (head->pktlen < end)
		head->pktlen = end;
	return 0;
}

void
mbuf_freem(mbuf_t m)
{
	mbuf_t next;

	for (; m != NULL; m = next) {
		next = m->next;
		live--;
		free(m);
	}
}

mbuf_t
fake_chain(const void *data, size_t len, size_t seg, int pkthdr)
{
	const unsigned char *p = data;
	mbuf_t head = NULL, *link = &head, m;
	size_t take;

	while (len > 0) {
		take = len < seg ? len : seg;
		m = seg_new(seg);
		if (m == NULL) {
			mbuf_freem(head);
			return NULL;
		}
		memcpy(m->data, p, take);
		m->len = take;
		p += take;
		len -= take;
		*link = m;
		link = &m->next;
	}
	if (head != NULL && pkthdr) {
		head->flags = MBUF_PKTHDR;
		head->pktlen = (size_t)(p - (const unsigned char *)data);
	}
	return head;
}

void
fake_set_pktlen(mbuf_t m, size_t len)
{
	m->pktlen = len;
}
