/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include "pie80211_ifops.h"

#define PIE80211_HDR	16

const uint8_t etherbroadcastaddr[ETHER_ADDR_LEN] = { [0 ... ETHER_ADDR_LEN - 1] = 0xff };
struct taskq *const systq = NULL;

static struct timeout *to_first;
static struct task *tk_first;
static struct task **tk_lastp = &tk_first;
static u_int tk_count;

void *
pie80211_malloc(size_t size, int type, int flags)
{
	size_t total;
	char *p;

	if (__builtin_add_overflow(size, (size_t)PIE80211_HDR, &total))
		return NULL;
	p = pie80211_alloc(total, ISSET(flags, M_ZERO));
	if (p == NULL)
		return NULL;
	memcpy(p, &total, sizeof(total));
	return p + PIE80211_HDR;
}

void
pie80211_free(void *ptr, int type, size_t size)
{
	size_t total;
	char *p;

	if (ptr == NULL)
		return;
	p = (char *)ptr - PIE80211_HDR;
	memcpy(&total, p, sizeof(total));
	pie80211_release(p, total);
}

void
pie80211_explicit_bzero(void *p, size_t n)
{
	memset(p, 0, n);
	__asm__ __volatile__("" : : "r"(p) : "memory");
}

int
pie80211_timingsafe_bcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	uint8_t diff = 0;

	while (n-- > 0)
		diff |= *x++ ^ *y++;
	return diff != 0;
}

char *
pie80211_ether_sprintf(const uint8_t *a)
{
	static char ring[4][18];
	static u_int slot;
	char *s = ring[slot++ & 3];

	snprintf(s, sizeof(ring[0]), "%02x:%02x:%02x:%02x:%02x:%02x",
	    a[0], a[1], a[2], a[3], a[4], a[5]);
	return s;
}

int
pie80211_copyin(const void *u, void *k, size_t n)
{
	return EOPNOTSUPP;
}

uint32_t
pie80211_arc4random(void)
{
	uint32_t v;

	pie80211_arc4random_buf(&v, sizeof(v));
	return v;
}

void
pie80211_getmicrouptime(struct timeval *tv)
{
	uint64_t ns = pie80211_nsecuptime();

	tv->tv_sec = (time_t)(ns / 1000000000ULL);
	tv->tv_usec = (suseconds_t)(ns % 1000000000ULL / 1000ULL);
}

int
pie80211_ifq_enqueue(struct ifqueue *ifq, struct mbuf *m)
{
	if (ifq->q_pkts.mh_count < ifq->q_limit) {
		pie80211_ml_enqueue(&ifq->q_pkts, m);
		return 0;
	}
	ifq->q_drops++;
	pie80211_m_freem(m);
	return ENOBUFS;
}

struct mbuf *
pie80211_ifq_dequeue(struct ifqueue *ifq)
{
	return pie80211_ml_dequeue(&ifq->q_pkts);
}

void
pie80211_if_attach(struct ifnet *ifp)
{
	ifp->if_snd = (struct ifqueue){ .q_limit = IFQ_MAXLEN };
}

void
pie80211_if_detach(struct ifnet *ifp)
{
	pie80211_ml_purge(&ifp->if_snd.q_pkts);
}

void
pie80211_if_start(struct ifnet *ifp)
{
	if (ifp->if_start != NULL)
		(*ifp->if_start)(ifp);
}

int
pie80211_if_enqueue(struct ifnet *ifp, struct mbuf *m)
{
	int error = pie80211_ifq_enqueue(&ifp->if_snd, m);

	if (error == 0)
		pie80211_if_start(ifp);
	else
		ifp->if_oerrors++;
	return error;
}

int
pie80211_ether_output(struct ifnet *ifp, struct mbuf *m0, struct sockaddr *sa,
    struct rtentry *route)
{
	ifp->if_oerrors++;
	pie80211_m_freem(m0);
	return EOPNOTSUPP;
}

void
pie80211_if_input(struct ifnet *ifp, struct mbuf_list *ml)
{
	const struct pie80211_ifops *ops = ifp->if_pie;

	if (ops == NULL || ops->input == NULL) {
		pie80211_ml_purge(ml);
		return;
	}
	ops->input(ifp, ml);
}

void
pie80211_if_link_state_change(struct ifnet *ifp)
{
	const struct pie80211_ifops *ops = ifp->if_pie;

	if (ops != NULL && ops->link_state != NULL)
		ops->link_state(ifp);
}

void
pie80211_ifmedia_init(struct ifmedia *ifm, pie80211_ifm_change_t ch, pie80211_ifm_stat_t st)
{
	*ifm = (struct ifmedia){ .m_change = ch, .m_status = st };
	TAILQ_INIT(&ifm->m_words);
}

void
pie80211_ifmedia_add(struct ifmedia *ifm, uint64_t w)
{
	struct ifmedia_entry *e = pie80211_malloc(sizeof(*e), M_DEVBUF, M_NOWAIT | M_ZERO);

	if (e != NULL) {
		e->ifm_media = w;
		TAILQ_INSERT_TAIL(&ifm->m_words, e, me_link);
	}
}

void
pie80211_ifmedia_set(struct ifmedia *ifm, uint64_t w)
{
	struct ifmedia_entry *e;

	TAILQ_FOREACH(e, &ifm->m_words, me_link) {
		if (e->ifm_media != w)
			continue;
		ifm->ifm_cur = e;
		ifm->ifm_media = w;
		break;
	}
}

void
pie80211_ifmedia_delete_instance(struct ifmedia *ifm, uint64_t inst)
{
	struct ifmedia_entry *e, *next;

	for (e = TAILQ_FIRST(&ifm->m_words); e != NULL; e = next) {
		next = TAILQ_NEXT(e, me_link);
		if (inst != IFM_INST_ANY && IFM_INST(e->ifm_media) != inst)
			continue;
		TAILQ_REMOVE(&ifm->m_words, e, me_link);
		if (ifm->ifm_cur == e)
			ifm->ifm_cur = NULL;
		pie80211_free(e, M_DEVBUF, sizeof(*e));
	}
}

uint64_t
pie80211_ifmedia_baudrate(uint64_t w)
{
	static const uint32_t kbps[] = {
		[IFM_IEEE80211_DS1] = 1000, [IFM_IEEE80211_DS2] = 2000,
		[IFM_IEEE80211_DS5] = 5500, [IFM_IEEE80211_DS11] = 11000,
		[IFM_IEEE80211_DS22] = 22000, [IFM_IEEE80211_OFDM6] = 6000,
		[IFM_IEEE80211_OFDM9] = 9000, [IFM_IEEE80211_OFDM12] = 12000,
		[IFM_IEEE80211_OFDM18] = 18000, [IFM_IEEE80211_OFDM24] = 24000,
		[IFM_IEEE80211_OFDM36] = 36000, [IFM_IEEE80211_OFDM48] = 48000,
		[IFM_IEEE80211_OFDM54] = 54000, [IFM_IEEE80211_OFDM72] = 72000,
	};
	uint64_t sub = IFM_SUBTYPE(w);

	if (IFM_TYPE(w) == IFM_IEEE80211 && sub < nitems(kbps))
		return kbps[sub] * 1000ULL;
	return 0;
}

void
pie80211_timeout_set(struct timeout *tmo, void (*cb)(void *), void *cookie)
{
	*tmo = (struct timeout){ .pt_fn = cb, .pt_arg = cookie, .pt_state = PIE80211_TO_SET };
}

static void
to_disarm(struct timeout *to)
{
	if (to->pt_next != NULL)
		to->pt_next->pt_link = to->pt_link;
	*to->pt_link = to->pt_next;
	to->pt_next = NULL;
	to->pt_link = NULL;
	CLR(to->pt_state, PIE80211_TO_ARMED);
}

int
pie80211_timeout_add_nsec(struct timeout *to, uint64_t nsec)
{
	struct timeout **at;
	int was;

	pie80211_lock();
	was = timeout_pending(to);
	if (was)
		to_disarm(to);
	to->pt_when = pie80211_nsecuptime() + nsec;
	at = &to_first;
	while (*at != NULL && (*at)->pt_when <= to->pt_when)
		at = &(*at)->pt_next;
	if ((to->pt_next = *at) != NULL)
		to->pt_next->pt_link = &to->pt_next;
	to->pt_link = at;
	*at = to;
	SET(to->pt_state, PIE80211_TO_SET | PIE80211_TO_ARMED);
	pie80211_unlock();
	pie80211_wake();
	return !was;
}

int
pie80211_timeout_del(struct timeout *to)
{
	int was;

	pie80211_lock();
	was = timeout_pending(to);
	if (was)
		to_disarm(to);
	pie80211_unlock();
	return was;
}

uint64_t
pie80211_timeouts_next(void)
{
	if (tk_first != NULL)
		return 0;
	return to_first != NULL ? to_first->pt_when : UINT64_MAX;
}

static void
run_tasks(void)
{
	struct task *t;
	void (*fn)(void *);
	void *arg;
	u_int batch;

	pie80211_lock();
	for (batch = tk_count; batch > 0 && (t = tk_first) != NULL; batch--) {
		if ((tk_first = t->tk_next) != NULL)
			tk_first->tk_link = &tk_first;
		else
			tk_lastp = &tk_first;
		t->tk_next = NULL;
		t->tk_link = NULL;
		t->tk_queued = 0;
		tk_count--;
		fn = t->tk_fn;
		arg = t->tk_arg;
		pie80211_unlock();
		fn(arg);
		pie80211_lock();
	}
	pie80211_unlock();
}

static void
run_timeouts(void)
{
	struct timeout *to;
	void (*fn)(void *);
	void *arg;
	uint64_t now = pie80211_nsecuptime();

	pie80211_lock();
	while ((to = to_first) != NULL && to->pt_when <= now) {
		to_disarm(to);
		fn = to->pt_fn;
		arg = to->pt_arg;
		pie80211_unlock();
		fn(arg);
		pie80211_lock();
	}
	pie80211_unlock();
}

void
pie80211_run_pending(void *arg)
{
	run_tasks();
	run_timeouts();
}

void
pie80211_task_set(struct task *job, void (*cb)(void *), void *cookie)
{
	*job = (struct task){ .tk_fn = cb, .tk_arg = cookie };
}

int
pie80211_task_add(struct taskq *tq, struct task *t)
{
	int added;

	pie80211_lock();
	added = !t->tk_queued;
	if (added) {
		t->tk_next = NULL;
		t->tk_link = tk_lastp;
		*tk_lastp = t;
		tk_lastp = &t->tk_next;
		t->tk_queued = 1;
		tk_count++;
	}
	pie80211_unlock();
	if (added)
		pie80211_wake();
	return added;
}

int
pie80211_task_del(struct taskq *tq, struct task *t)
{
	int was;

	pie80211_lock();
	was = t->tk_queued;
	if (was) {
		if (t->tk_next != NULL)
			t->tk_next->tk_link = t->tk_link;
		else
			tk_lastp = t->tk_link;
		*t->tk_link = t->tk_next;
		t->tk_next = NULL;
		t->tk_link = NULL;
		t->tk_queued = 0;
		tk_count--;
	}
	pie80211_unlock();
	return was;
}
