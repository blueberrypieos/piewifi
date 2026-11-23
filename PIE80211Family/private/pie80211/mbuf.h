/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_MBUF_H
#define _PIE80211_MBUF_H

#include <pie80211/base.h>
#include <pie80211/kern.h>
#include <pie80211/family.h>

#define mbuf			pie80211_mbuf
#define pkthdr			pie80211_pkthdr
#define m_tag			pie80211_m_tag
#define mbuf_list		pie80211_mbuf_list
#define mbuf_queue		pie80211_mbuf_queue

#define MLEN			PIE80211_MLEN
#define MHLEN			PIE80211_MLEN
#undef MCLBYTES
#define MCLBYTES		2048
#define MINCLSIZE		(MHLEN + 1)

#define M_PKTHDR		0x01
#define M_EXT			0x02
#define M_MCAST			0x04
#define PIE80211_M_HDRFLAGS	(M_PKTHDR | M_MCAST)
#define MT_DATA			1
#define PACKET_TAG_DLT		1

struct m_tag {
	uint32_t mt_type;
};

struct pkthdr {
	void *ph_cookie;
	int len;
	uint32_t ph_rxflags;
	uint32_t ph_tstamp;
	uint16_t ph_freq;
	int8_t ph_rssi;
};

struct pie80211_mext {
	char *ext_buf;
	u_int ext_size;
};

struct mbuf {
	struct mbuf *m_next;
	struct mbuf *m_nextpkt;
	char *m_data;
	u_int m_len;
	int m_type;
	int m_flags;
	struct pkthdr m_pkthdr;
	struct pie80211_mext m_ext;
	char m_dat[PIE80211_MLEN];
};

#define mtod(mb, type)			((type)(void *)(mb)->m_data)
#define m_get(how, type)		pie80211_m_get(how, type)
#define m_gethdr(how, type)		pie80211_m_gethdr(how, type)
#define m_clget(m, how, size)		pie80211_m_clget(m, how, size)
#define m_free(m)			pie80211_m_free(m)
#define m_freem(m)			pie80211_m_freem(m)
#define m_adj(m, n)			pie80211_m_adj(m, n)
#define m_pullup(m, n)			pie80211_m_pullup(m, n)
#define m_prepend(m, n, how)		pie80211_m_prepend(m, n, how)
#define m_copydata(m, off, n, p)	pie80211_m_copydata(m, off, n, p)
#define m_align(m, n)			pie80211_m_align(m, n)
#define m_trailingspace(m)		pie80211_m_trailingspace(m)
#define m_split(m, n, how)		pie80211_m_split(m, n, how)
#define m_cat(m, n)			pie80211_m_cat(m, n)
#define m_dup_pkthdr(to, from, how)	pie80211_m_dup_pkthdr(to, from, how)
#define m_dup_pkt(m, adj, how)		pie80211_m_dup_pkt(m, adj, how)
#define m_tag_find(m, type, prev)	pie80211_m_tag_find(m, type, prev)
#define MGET(mp, wait, kind)		((mp) = pie80211_m_get(wait, kind))
#define MGETHDR(mp, wait, kind)		((mp) = pie80211_m_gethdr(wait, kind))
#define MCLGET(m, how)			((void)pie80211_m_clget(m, how, MCLBYTES))
#define M_PREPEND(m, n, how)		((m) = pie80211_m_prepend(m, n, how))

struct mbuf *pie80211_m_get(int how, int type);
struct mbuf *pie80211_m_gethdr(int how, int type);
struct mbuf *pie80211_m_clget(struct mbuf *m, int how, u_int size);
struct mbuf *pie80211_m_free(struct mbuf *m);
void pie80211_m_adj(struct mbuf *m, int n);
struct mbuf *pie80211_m_pullup(struct mbuf *m, int n);
struct mbuf *pie80211_m_prepend(struct mbuf *m, int n, int how);
void pie80211_m_copydata(struct mbuf *m, int off, int n, void *p);
void pie80211_m_align(struct mbuf *m, int n);
int pie80211_m_leadingspace(struct mbuf *m);
int pie80211_m_trailingspace(struct mbuf *m);
struct mbuf *pie80211_m_split(struct mbuf *m, int n, int how);
void pie80211_m_cat(struct mbuf *m, struct mbuf *n);
int pie80211_m_dup_pkthdr(struct mbuf *to, struct mbuf *from, int how);
struct mbuf *pie80211_m_dup_pkt(struct mbuf *m, u_int adj, int how);
struct m_tag *pie80211_m_tag_find(struct mbuf *m, int type, struct m_tag *prev);

struct mbuf_list {
	struct mbuf *mh_head;
	struct mbuf **mh_tailp;
	u_int mh_count;
};
#define MBUF_LIST_INITIALIZER()		{ NULL, NULL, 0 }
#define ml_enqueue(ml, m)		pie80211_ml_enqueue(ml, m)
#define ml_dequeue(ml)			pie80211_ml_dequeue(ml)
#define ml_purge(ml)			pie80211_ml_purge(ml)
void pie80211_ml_enqueue(struct mbuf_list *ml, struct mbuf *m);
struct mbuf *pie80211_ml_dequeue(struct mbuf_list *ml);
u_int pie80211_ml_purge(struct mbuf_list *ml);

struct mbuf_queue {
	struct mbuf_list mq_pkts;
	u_int mq_limit;
	u_int mq_drops;
};
#define MBUF_QUEUE_INITIALIZER(lim, ipl) { MBUF_LIST_INITIALIZER(), (lim), 0 }
#define mq_init(mq, lim, ipl)		pie80211_mq_init(mq, lim)
#define mq_enqueue(mq, m)		pie80211_mq_enqueue(mq, m)
#define mq_dequeue(mq)			pie80211_ml_dequeue(&(mq)->mq_pkts)
#define mq_purge(mq)			pie80211_ml_purge(&(mq)->mq_pkts)
#define mq_len(mq)			((mq)->mq_pkts.mh_count)
#define mq_empty(mq)			((mq)->mq_pkts.mh_count == 0)
void pie80211_mq_init(struct mbuf_queue *mq, u_int lim);
int pie80211_mq_enqueue(struct mbuf_queue *mq, struct mbuf *m);

#endif
