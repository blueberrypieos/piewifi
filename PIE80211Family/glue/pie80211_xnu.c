/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/param.h>
#include <IOKit/IOLib.h>
#include <pie80211/family_xnu.h>
#include <pie80211/platform.h>

static lck_grp_t *pie_grp;
static lck_mtx_t *pie_mtx;
static lck_mtx_t *pie_gate_mtx;
static bool pie_timer_on;
static bool pie_stopping;
static void (*pie_gate)(void *, void (*)(void *), void *);
static bool (*pie_in_gate)(void *);
static void *pie_gate_ctx;
static unsigned int pie_gate_users;
static bool pie_gate_closing;
static thread_t pie_gate_owner;

kern_return_t pie80211_kext_start(kmod_info_t *ki, void *data);
kern_return_t pie80211_kext_stop(kmod_info_t *ki, void *data);

void *
pie80211_alloc(size_t size, bool zero)
{
	return zero ? IOMallocZero(size) : IOMalloc(size);
}

void
pie80211_release(void *p, size_t size)
{
	IOFree(p, size);
}

uint64_t
pie80211_nsecuptime(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

void
pie80211_arc4random_buf(void *p, size_t n)
{
	read_random(p, (u_int)n);
}

void
pie80211_kassert(const char *expr, const char *file, int line)
{
	panic("net80211: assertion \"%s\" failed at %s:%d", expr, file, line);
}

int
pie80211_splnet(void)
{
	return 0;
}

void
pie80211_splx(int s)
{
	(void)s;
}

void
pie80211_lock(void)
{
	lck_mtx_lock(pie_mtx);
}

void
pie80211_unlock(void)
{
	lck_mtx_unlock(pie_mtx);
}

static void
pie80211_arm(void)
{
	uint64_t next, deadline;

	if (pie_stopping || !pie_timer_on)
		return;
	next = pie80211_timeouts_next();
	if (next == UINT64_MAX)
		return;
	if (next <= pie80211_nsecuptime()) {
		pie80211_timer_arm(0);
		return;
	}
	nanoseconds_to_absolutetime(next, &deadline);
	pie80211_timer_arm(deadline);
}

void
pie80211_wake(void)
{
	lck_mtx_lock(pie_mtx);
	pie80211_arm();
	lck_mtx_unlock(pie_mtx);
}

void
pie80211_splassert(const char *func)
{
	bool (*in_gate)(void *);
	void *ctx;
	bool held;

	lck_mtx_lock(pie_mtx);
	in_gate = pie_in_gate;
	ctx = pie_gate_ctx;
	lck_mtx_unlock(pie_mtx);
	held = in_gate != NULL ? in_gate(ctx) : pie_gate_owner == current_thread();
	if (!held)
		printf("pie80211: %s runs outside the gate\n", func);
}

int
pie80211_set_gate(void (*gate)(void *, void (*)(void *), void *), bool (*in_gate)(void *),
    void *ctx)
{
	lck_mtx_lock(pie_mtx);
	if (gate != NULL) {
		if (pie_gate != NULL) {
			lck_mtx_unlock(pie_mtx);
			return EBUSY;
		}
		pie_gate = gate;
		pie_in_gate = in_gate;
		pie_gate_ctx = ctx;
		lck_mtx_unlock(pie_mtx);
		return 0;
	}
	pie_gate_closing = true;
	while (pie_gate_users > 0)
		(void)msleep(&pie_gate_users, pie_mtx, PZERO, "pie80211", NULL);
	pie_gate = NULL;
	pie_in_gate = NULL;
	pie_gate_ctx = NULL;
	pie_gate_closing = false;
	lck_mtx_unlock(pie_mtx);
	return 0;
}

void
pie80211_gated(void (*fn)(void *), void *arg)
{
	void (*gate)(void *, void (*)(void *), void *) = NULL;
	void *ctx = NULL;

	lck_mtx_lock(pie_mtx);
	if (pie_gate != NULL && !pie_gate_closing) {
		gate = pie_gate;
		ctx = pie_gate_ctx;
		pie_gate_users++;
	}
	lck_mtx_unlock(pie_mtx);
	if (gate != NULL) {
		gate(ctx, fn, arg);
		lck_mtx_lock(pie_mtx);
		if (--pie_gate_users == 0 && pie_gate_closing)
			wakeup(&pie_gate_users);
		lck_mtx_unlock(pie_mtx);
		return;
	}
	lck_mtx_lock(pie_gate_mtx);
	pie_gate_owner = current_thread();
	fn(arg);
	pie_gate_owner = NULL;
	lck_mtx_unlock(pie_gate_mtx);
}

static void
pie80211_fire(void)
{
	pie80211_gated(pie80211_run_pending, NULL);
	pie80211_wake();
}

kern_return_t
pie80211_kext_start(kmod_info_t *ki, void *data)
{
	pie_grp = lck_grp_alloc_init("pie80211", LCK_GRP_ATTR_NULL);
	if (pie_grp == NULL)
		return KERN_FAILURE;
	pie_mtx = lck_mtx_alloc_init(pie_grp, LCK_ATTR_NULL);
	pie_gate_mtx = lck_mtx_alloc_init(pie_grp, LCK_ATTR_NULL);
	if (pie_mtx != NULL && pie_gate_mtx != NULL && pie80211_timer_start(pie80211_fire) == 0) {
		pie_stopping = false;
		pie_timer_on = true;
		return KERN_SUCCESS;
	}
	if (pie_gate_mtx != NULL)
		lck_mtx_free(pie_gate_mtx, pie_grp);
	if (pie_mtx != NULL)
		lck_mtx_free(pie_mtx, pie_grp);
	lck_grp_free(pie_grp);
	pie_gate_mtx = pie_mtx = NULL;
	pie_grp = NULL;
	return KERN_FAILURE;
}

kern_return_t
pie80211_kext_stop(kmod_info_t *ki, void *data)
{
	lck_mtx_lock(pie_mtx);
	pie_stopping = true;
	pie_timer_on = false;
	lck_mtx_unlock(pie_mtx);
	pie80211_timer_stop();
	lck_mtx_free(pie_gate_mtx, pie_grp);
	lck_mtx_free(pie_mtx, pie_grp);
	lck_grp_free(pie_grp);
	return KERN_SUCCESS;
}
