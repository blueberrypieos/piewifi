/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _HOST_H
#define _HOST_H

#include <stdint.h>
#include <stdio.h>

extern int host_failures;
extern void (*host_gate_hook)(void);
extern void (*host_gate_exit_hook)(void);

#define CHECK(e)							\
	do {								\
		if (!(e)) {						\
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); \
			host_failures++;				\
		}							\
	} while (0)

void host_fail_after(long n);
long host_live(void);
void host_advance(uint64_t ns);
int host_report(const char *name);

#endif
