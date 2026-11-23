/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pie80211/platform.h>
#include "host.h"

int host_failures;
void (*host_gate_hook)(void);
void (*host_gate_exit_hook)(void);
static int in_gate_hook;
static int gate_depth;
static long live;
static long fail_in = -1;
static uint64_t clock_ns = 1000000000ULL;
static uint64_t rand_state = 0x9e3779b97f4a7c15ULL;

void
host_fail_after(long n)
{
	fail_in = n;
}

long
host_live(void)
{
	return live;
}

void
host_advance(uint64_t ns)
{
	clock_ns += ns;
}

int
host_report(const char *name)
{
	if (live != 0) {
		fprintf(stderr, "%s: %ld allocations left\n", name, live);
		host_failures++;
	}
	printf("%s: %s\n", name, host_failures == 0 ? "ok" : "FAILED");
	return host_failures != 0;
}

void *
pie80211_alloc(size_t size, bool zero)
{
	void *p;

	if (fail_in == 0) {
		fail_in = -1;
		return NULL;
	}
	if (fail_in > 0)
		fail_in--;
	p = zero ? calloc(1, size) : malloc(size);
	if (p != NULL)
		live++;
	return p;
}

void
pie80211_release(void *p, size_t size)
{
	if (p != NULL) {
		live--;
		free(p);
	}
}

uint64_t
pie80211_nsecuptime(void)
{
	return clock_ns;
}

void
pie80211_arc4random_buf(void *p, size_t n)
{
	uint8_t *out = p;

	while (n-- > 0) {
		rand_state ^= rand_state << 13;
		rand_state ^= rand_state >> 7;
		rand_state ^= rand_state << 17;
		*out++ = (uint8_t)rand_state;
	}
}

void
pie80211_kassert(const char *expr, const char *file, int line)
{
	fprintf(stderr, "assertion \"%s\" failed at %s:%d\n", expr, file, line);
	abort();
}

void
panic(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	abort();
}

int
pie80211_splnet(void)
{
	return 0;
}

void
pie80211_splx(int s)
{
}

void
pie80211_splassert(const char *func)
{
}

void
pie80211_lock(void)
{
}

void
pie80211_unlock(void)
{
}

void
pie80211_wake(void)
{
}

void
pie80211_gated(void (*fn)(void *), void *arg)
{
	if (host_gate_hook != NULL && !in_gate_hook) {
		in_gate_hook = 1;
		host_gate_hook();
		in_gate_hook = 0;
	}
	gate_depth++;
	fn(arg);
	gate_depth--;
	if (host_gate_exit_hook != NULL && !in_gate_hook && gate_depth == 0) {
		in_gate_hook = 1;
		host_gate_exit_hook();
		in_gate_hook = 0;
	}
}
