/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <pie80211/family_xnu.h>

static IOWorkLoop *pie_timer_loop;
static IOTimerEventSource *pie_timer;
static void (*pie_timer_fn)(void);

static void
pie_timer_fired(OSObject *owner, IOTimerEventSource *sender)
{
	pie_timer_fn();
}

int
pie80211_timer_start(void (*fn)(void))
{
	pie_timer_fn = fn;
	pie_timer_loop = IOWorkLoop::workLoop();
	if (pie_timer_loop != NULL)
		pie_timer = IOTimerEventSource::timerEventSource(pie_timer_loop, pie_timer_fired);
	if (pie_timer == NULL || pie_timer_loop->addEventSource(pie_timer) != kIOReturnSuccess) {
		OSSafeReleaseNULL(pie_timer);
		OSSafeReleaseNULL(pie_timer_loop);
		return ENOMEM;
	}
	return 0;
}

void
pie80211_timer_arm(uint64_t deadline)
{
	(void)pie_timer->wakeAtTime(deadline != 0 ? deadline : mach_absolute_time());
}

void
pie80211_timer_stop(void)
{
	pie_timer->cancelTimeout();
	pie_timer_loop->removeEventSource(pie_timer);
	OSSafeReleaseNULL(pie_timer);
	OSSafeReleaseNULL(pie_timer_loop);
}
