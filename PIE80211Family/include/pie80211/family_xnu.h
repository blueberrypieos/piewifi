/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_FAMILY_XNU_H
#define _PIE80211_FAMILY_XNU_H

#include <stdbool.h>
#include <sys/kpi_mbuf.h>
#include <pie80211/family.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pie80211_mbuf *pie80211_m_from_xnu(mbuf_t xm, size_t lead);
int pie80211_m_to_xnu(struct pie80211_mbuf *m, mbuf_t *out);
int pie80211_set_gate(void (*gate)(void *ctx, void (*fn)(void *), void *arg),
    bool (*in_gate)(void *ctx), void *ctx);

/*
 * The family's timer: an IOTimerEventSource on a work loop of its own.
 * pie80211_timer_arm sets its one deadline, in absolute time, 0 for at
 * once. At it, fn runs on a kernel thread, from the timer's thread call,
 * with the loop's gate held. pie80211_timer_stop cancels it and waits for
 * a run of fn in progress.
 */
int pie80211_timer_start(void (*fn)(void));
void pie80211_timer_arm(uint64_t deadline);
void pie80211_timer_stop(void);

#ifdef __cplusplus
}
#endif

#endif
