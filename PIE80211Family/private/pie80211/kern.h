/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_KERN_H
#define _PIE80211_KERN_H

#include <pie80211/base.h>

#define M_NOWAIT		0x01
#define M_ZERO			0x02
#define M_DONTWAIT		M_NOWAIT
#define M_DEVBUF		1
#define IPL_NET			0

#define malloc(size, type, flags)	pie80211_malloc(size, type, flags)
#define free(p, type, size)		pie80211_free(p, type, size)
void *pie80211_malloc(size_t size, int type, int flags);
void pie80211_free(void *p, int type, size_t size);

#define splnet()			pie80211_splnet()
#define splx(s)				pie80211_splx(s)
#define splassert(ipl)			pie80211_splassert(__func__)

#undef copyin
#define copyin(u, k, n)			pie80211_copyin(u, k, n)
int pie80211_copyin(const void *u, void *k, size_t n);

#define arc4random()			pie80211_arc4random()
#define arc4random_buf(p, n)		pie80211_arc4random_buf(p, n)
uint32_t pie80211_arc4random(void);

#undef getmicrouptime
#define getmicrouptime(tv)		pie80211_getmicrouptime(tv)
void pie80211_getmicrouptime(struct timeval *tv);

#define timeout				pie80211_timeout
struct timeout {
	struct timeout *pt_next;
	struct timeout **pt_link;
	uint64_t pt_when;
	void (*pt_fn)(void *);
	void *pt_arg;
	int pt_state;
};
#define PIE80211_TO_SET			1
#define PIE80211_TO_ARMED		2
#define timeout_set(to, fn, arg)	pie80211_timeout_set(to, fn, arg)
#define timeout_add_sec(to, s)		pie80211_timeout_add_nsec(to, (uint64_t)(s) * 1000000000ULL)
#define timeout_add_msec(to, ms)	pie80211_timeout_add_nsec(to, (uint64_t)(ms) * 1000000ULL)
#define timeout_add_usec(to, us)	pie80211_timeout_add_nsec(to, (uint64_t)(us) * 1000ULL)
#define timeout_del(to)			pie80211_timeout_del(to)
#define timeout_pending(to)		(((to)->pt_state & PIE80211_TO_ARMED) != 0)
#define timeout_initialized(to)		(((to)->pt_state & PIE80211_TO_SET) != 0)
void pie80211_timeout_set(struct timeout *, void (*)(void *), void *);
int pie80211_timeout_add_nsec(struct timeout *to, uint64_t nsec);
int pie80211_timeout_del(struct timeout *to);

#define task				pie80211_task
#define taskq				pie80211_taskq
struct task {
	struct task *tk_next;
	struct task **tk_link;
	void (*tk_fn)(void *);
	void *tk_arg;
	int tk_queued;
};
struct taskq;
#define systq				pie80211_systq
extern struct taskq *const systq;
#define task_set(t, fn, arg)		pie80211_task_set(t, fn, arg)
#define task_add(tq, t)			pie80211_task_add(tq, t)
#define task_del(tq, t)			pie80211_task_del(tq, t)
void pie80211_task_set(struct task *, void (*)(void *), void *);
int pie80211_task_add(struct taskq *tq, struct task *t);
int pie80211_task_del(struct taskq *tq, struct task *t);

#endif
