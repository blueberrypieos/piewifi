/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>

static char *
store_of(struct mbuf *m)
{
	return ISSET(m->m_flags, M_EXT) ? m->m_ext.ext_buf : m->m_dat;
}

static u_int
store_size(struct mbuf *m)
{
	return ISSET(m->m_flags, M_EXT) ? m->m_ext.ext_size : PIE80211_MLEN;
}

static struct mbuf *
mbuf_new(int type, int flags)
{
	struct mbuf *m = pie80211_alloc(sizeof(*m), true);

	if (m != NULL) {
		m->m_type = type;
		m->m_flags = flags;
		m->m_data = m->m_dat;
	}
	return m;
}

static void
adopt_header(struct mbuf *n, struct mbuf *m)
{
	if (ISSET(m->m_flags, M_PKTHDR)) {
		n->m_pkthdr = m->m_pkthdr;
		SET(n->m_flags, m->m_flags & PIE80211_M_HDRFLAGS);
		CLR(m->m_flags, PIE80211_M_HDRFLAGS);
		m->m_pkthdr = (struct pkthdr){ 0 };
	}
	n->m_nextpkt = m->m_nextpkt;
	m->m_nextpkt = NULL;
	n->m_next = m;
}

struct mbuf *
pie80211_m_get(int how, int type)
{
	return mbuf_new(type, 0);
}

struct mbuf *
pie80211_m_gethdr(int how, int type)
{
	return mbuf_new(type, M_PKTHDR);
}

struct mbuf *
pie80211_m_clget(struct mbuf *m, int how, u_int size)
{
	struct mbuf *own = NULL;
	char *store;

	if (size == 0 || size > PIE80211_MAXEXT)
		return NULL;
	if (m == NULL && (m = own = mbuf_new(MT_DATA, M_PKTHDR)) == NULL)
		return NULL;
	if (ISSET(m->m_flags, M_EXT))
		return m;
	store = pie80211_alloc(size, false);
	if (store == NULL) {
		if (own != NULL)
			pie80211_m_free(own);
		return NULL;
	}
	m->m_ext = (struct pie80211_mext){ .ext_buf = store, .ext_size = size };
	m->m_data = store;
	SET(m->m_flags, M_EXT);
	return m;
}

struct mbuf *
pie80211_m_free(struct mbuf *m)
{
	struct mbuf *rest;

	if (m == NULL)
		return NULL;
	rest = m->m_next;
	if (ISSET(m->m_flags, M_EXT))
		pie80211_release(m->m_ext.ext_buf, m->m_ext.ext_size);
	pie80211_release(m, sizeof(*m));
	return rest;
}

struct mbuf *
pie80211_m_freem(struct mbuf *m)
{
	struct mbuf *after = m != NULL ? m->m_nextpkt : NULL;

	for (; m != NULL; m = pie80211_m_free(m))
		;
	return after;
}

int
pie80211_m_leadingspace(struct mbuf *m)
{
	return (int)(m->m_data - store_of(m));
}

int
pie80211_m_trailingspace(struct mbuf *m)
{
	return (int)store_size(m) - pie80211_m_leadingspace(m) - (int)m->m_len;
}

void
pie80211_m_align(struct mbuf *m, int n)
{
	u_int size = store_size(m);

	if (n >= 0 && (u_int)n <= size)
		m->m_data = store_of(m) + ((size - (u_int)n) & ~(u_int)(sizeof(long) - 1));
}

void
pie80211_m_adj(struct mbuf *mp, int req)
{
	struct mbuf *m;
	int cut, keep, total;

	if (mp == NULL)
		return;
	if (req >= 0) {
		for (m = mp, keep = req; m != NULL && keep > 0; m = m->m_next) {
			cut = MIN(keep, (int)m->m_len);
			m->m_data += cut;
			m->m_len -= (u_int)cut;
			keep -= cut;
		}
		if (ISSET(mp->m_flags, M_PKTHDR))
			mp->m_pkthdr.len -= req - keep;
		return;
	}
	for (m = mp, total = 0; m != NULL; m = m->m_next)
		total += (int)m->m_len;
	keep = MAX(total + req, 0);
	if (ISSET(mp->m_flags, M_PKTHDR))
		mp->m_pkthdr.len = keep;
	for (m = mp; m != NULL; m = m->m_next) {
		m->m_len = (u_int)MIN((int)m->m_len, keep);
		keep -= (int)m->m_len;
	}
}

struct mbuf *
pie80211_m_pullup(struct mbuf *m, int n)
{
	struct mbuf *dst, *src;
	int need, take;

	if (m == NULL || n <= (int)m->m_len)
		return m;
	if (n < 0 || n > PIE80211_MAXEXT)
		goto fail;
	if (m->m_next != NULL &&
	    (int)store_size(m) - pie80211_m_leadingspace(m) >= n) {
		dst = m;
	} else {
		dst = mbuf_new(m->m_type, 0);
		if (dst == NULL)
			goto fail;
		if (n > PIE80211_MLEN && pie80211_m_clget(dst, M_DONTWAIT, (u_int)n) == NULL) {
			pie80211_m_free(dst);
			goto fail;
		}
		adopt_header(dst, m);
		m = dst;
	}
	for (need = n - (int)dst->m_len, src = dst->m_next; need > 0 && src != NULL;) {
		take = MIN(need, (int)src->m_len);
		memcpy(dst->m_data + dst->m_len, src->m_data, (size_t)take);
		dst->m_len += (u_int)take;
		src->m_data += take;
		src->m_len -= (u_int)take;
		need -= take;
		if (src->m_len == 0)
			src = dst->m_next = pie80211_m_free(src);
	}
	if (need == 0)
		return dst;
fail:
	pie80211_m_freem(m);
	return NULL;
}

struct mbuf *
pie80211_m_prepend(struct mbuf *m, int n, int how)
{
	struct mbuf *head;

	if (m == NULL)
		return NULL;
	if (n < 0 || n > PIE80211_MLEN) {
		pie80211_m_freem(m);
		return NULL;
	}
	if (n <= pie80211_m_leadingspace(m)) {
		head = m;
		head->m_data -= n;
		head->m_len += (u_int)n;
	} else {
		head = mbuf_new(m->m_type, 0);
		if (head == NULL) {
			pie80211_m_freem(m);
			return NULL;
		}
		adopt_header(head, m);
		pie80211_m_align(head, n);
		head->m_len = (u_int)n;
	}
	if (ISSET(head->m_flags, M_PKTHDR))
		head->m_pkthdr.len += n;
	return head;
}

void
pie80211_m_copydata(struct mbuf *m, int off, int n, void *p)
{
	uint8_t *out = p;
	int take;

	if (off < 0 || n <= 0)
		return;
	for (; m != NULL && n > 0; m = m->m_next) {
		if (off >= (int)m->m_len) {
			off -= (int)m->m_len;
			continue;
		}
		take = MIN(n, (int)m->m_len - off);
		memcpy(out, m->m_data + off, (size_t)take);
		out += take;
		n -= take;
		off = 0;
	}
	if (n > 0)
		memset(out, 0, (size_t)n);
}

struct mbuf *
pie80211_m_split(struct mbuf *m0, int len, int how)
{
	struct mbuf *m = m0, *tail;
	int at = len, rest;

	if (m0 == NULL || len < 0)
		return NULL;
	while (m != NULL && at > (int)m->m_len) {
		at -= (int)m->m_len;
		m = m->m_next;
	}
	if (m == NULL)
		return NULL;
	rest = (int)m->m_len - at;
	tail = mbuf_new(m0->m_type, 0);
	if (tail == NULL)
		return NULL;
	if (rest > PIE80211_MLEN && pie80211_m_clget(tail, how, (u_int)rest) == NULL) {
		pie80211_m_free(tail);
		return NULL;
	}
	if (ISSET(m0->m_flags, M_PKTHDR)) {
		pie80211_m_dup_pkthdr(tail, m0, how);
		tail->m_pkthdr.len = m0->m_pkthdr.len - len;
		m0->m_pkthdr.len = len;
	}
	memcpy(tail->m_data, m->m_data + at, (size_t)rest);
	tail->m_len = (u_int)rest;
	tail->m_next = m->m_next;
	m->m_next = NULL;
	m->m_len = (u_int)at;
	return tail;
}

void
pie80211_m_cat(struct mbuf *m, struct mbuf *n)
{
	struct mbuf **link = &m;

	KASSERT(m != NULL);
	while (*link != NULL)
		link = &(*link)->m_next;
	*link = n;
}

int
pie80211_m_dup_pkthdr(struct mbuf *to, struct mbuf *from, int how)
{
	to->m_flags = (to->m_flags & M_EXT) | (from->m_flags & PIE80211_M_HDRFLAGS) | M_PKTHDR;
	to->m_pkthdr = from->m_pkthdr;
	if (!ISSET(to->m_flags, M_EXT))
		to->m_data = to->m_dat;
	return 0;
}

struct mbuf *
pie80211_m_dup_pkt(struct mbuf *m0, u_int adj, int how)
{
	struct mbuf *m;
	void *data;
	int len;

	if (m0 == NULL || !ISSET(m0->m_flags, M_PKTHDR))
		return NULL;
	len = m0->m_pkthdr.len;
	if (len <= 0)
		return NULL;
	m = pie80211_m_alloc((size_t)len, adj, &data);
	if (m == NULL)
		return NULL;
	SET(m->m_flags, m0->m_flags & PIE80211_M_HDRFLAGS);
	m->m_pkthdr.ph_cookie = m0->m_pkthdr.ph_cookie;
	pie80211_m_copydata(m0, 0, len, data);
	return m;
}

struct m_tag *
pie80211_m_tag_find(struct mbuf *m, int type, struct m_tag *prev)
{
	return NULL;
}

void
pie80211_ml_enqueue(struct mbuf_list *ml, struct mbuf *m)
{
	m->m_nextpkt = NULL;
	*(ml->mh_tailp != NULL ? ml->mh_tailp : &ml->mh_head) = m;
	ml->mh_tailp = &m->m_nextpkt;
	ml->mh_count++;
}

struct mbuf *
pie80211_ml_dequeue(struct mbuf_list *ml)
{
	struct mbuf *m = ml->mh_head;

	if (m == NULL)
		return NULL;
	ml->mh_head = m->m_nextpkt;
	if (--ml->mh_count == 0)
		ml->mh_tailp = NULL;
	m->m_nextpkt = NULL;
	return m;
}

u_int
pie80211_ml_purge(struct mbuf_list *ml)
{
	u_int freed = 0;

	for (struct mbuf *m; (m = pie80211_ml_dequeue(ml)) != NULL; freed++)
		pie80211_m_freem(m);
	return freed;
}

void
pie80211_mq_init(struct mbuf_queue *mq, u_int lim)
{
	*mq = (struct mbuf_queue)MBUF_QUEUE_INITIALIZER(lim, IPL_NET);
}

int
pie80211_mq_enqueue(struct mbuf_queue *mq, struct mbuf *m)
{
	if (mq_len(mq) < mq->mq_limit) {
		pie80211_ml_enqueue(&mq->mq_pkts, m);
		return 0;
	}
	mq->mq_drops++;
	pie80211_m_freem(m);
	return ENOBUFS;
}

struct mbuf *
pie80211_m_alloc(size_t len, size_t lead, void **data)
{
	struct mbuf *m;

	if (lead > PIE80211_MLEN || len > PIE80211_MAXEXT - lead)
		return NULL;
	m = mbuf_new(MT_DATA, M_PKTHDR);
	if (m == NULL)
		return NULL;
	if (len + lead > PIE80211_MLEN && pie80211_m_clget(m, M_DONTWAIT, (u_int)(len + lead)) == NULL) {
		pie80211_m_free(m);
		return NULL;
	}
	m->m_data += lead;
	m->m_len = (u_int)len;
	m->m_pkthdr.len = (int)len;
	*data = m->m_data;
	return m;
}

struct mbuf *
pie80211_m_frombuf(const void *buf, size_t len, size_t lead)
{
	void *data;
	struct mbuf *m = pie80211_m_alloc(len, lead, &data);

	if (m != NULL)
		memcpy(data, buf, len);
	return m;
}

size_t
pie80211_m_pktlen(struct mbuf *m)
{
	if (m == NULL || !ISSET(m->m_flags, M_PKTHDR) || m->m_pkthdr.len < 0)
		return 0;
	return (size_t)m->m_pkthdr.len;
}

int
pie80211_m_foreach(struct mbuf *m, int (*fn)(void *, const void *, size_t), void *arg)
{
	size_t left = pie80211_m_pktlen(m), seg;
	int error;

	for (; m != NULL && left > 0; m = m->m_next) {
		seg = MIN((size_t)m->m_len, left);
		if (seg == 0)
			continue;
		if ((error = fn(arg, m->m_data, seg)) != 0)
			return error;
		left -= seg;
	}
	return left == 0 ? 0 : EINVAL;
}

int
pie80211_m_tobuf(struct mbuf *m, void *buf, size_t cap)
{
	size_t len = pie80211_m_pktlen(m);

	if (len == 0 || len > cap)
		return -1;
	pie80211_m_copydata(m, 0, (int)len, buf);
	return (int)len;
}
