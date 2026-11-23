/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_PLATFORM_H
#define _PIE80211_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *pie80211_alloc(size_t size, bool zero);
void pie80211_release(void *p, size_t size);
uint64_t pie80211_nsecuptime(void);
void pie80211_arc4random_buf(void *p, size_t n);
void pie80211_kassert(const char *expr, const char *file, int line);
int pie80211_splnet(void);
void pie80211_splx(int s);
void pie80211_splassert(const char *func);
void pie80211_lock(void);
void pie80211_unlock(void);
void pie80211_wake(void);
uint64_t pie80211_timeouts_next(void);
void pie80211_run_pending(void *arg);
void pie80211_gated(void (*fn)(void *), void *arg);

#endif
