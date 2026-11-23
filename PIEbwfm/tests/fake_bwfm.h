/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _FAKE_BWFM_H
#define _FAKE_BWFM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAKE_F1		((struct pie_sdio_func *)(uintptr_t)0x1001)
#define FAKE_F2		((struct pie_sdio_func *)(uintptr_t)0x1002)
#define FAKE_CALLS	64
#define FAKE_PROPS	8
#define FAKE_FILES	4
#define FAKE_READS	16

enum {
	FAKE_READ1 = 1, FAKE_WRITE1, FAKE_READ4, FAKE_WRITE4, FAKE_READ, FAKE_WRITE,
	FAKE_ENABLE, FAKE_DISABLE, FAKE_BLOCK, FAKE_IRQ_ON, FAKE_IRQ_OFF, FAKE_RESET,
};

struct fake_sdio_call {
	int op;
	unsigned int fn;
	uint32_t addr;
	size_t len;
	bool incr;
};

struct fake_sdio {
	struct fake_sdio_call calls[FAKE_CALLS];
	int ncalls;
	int claims;
	int releases;
	int reset_error;
	int resets_claimed;
	unsigned int generation;
	int set_up_on_unregister;
	int reset_at_register;
	unsigned int reset_at_release;
	uint8_t read1_value;
	uint32_t read4_value;
	bool no_f2;
	void (*handler)(void *);
	void *handler_arg;
	void (*reset_handler)(void *);
	void *reset_arg;
};

struct fake_prop {
	void *node;
	const char *name;
	const void *data;
	size_t len;
};

struct fake_file {
	const char *path;
	const void *data;
	size_t len;
};

extern struct fake_sdio fake_sdio;
extern struct fake_prop fake_props[FAKE_PROPS];
extern struct fake_file fake_files[FAKE_FILES];
extern int fake_reads;
extern char fake_read_path[FAKE_READS][128];
extern int fake_kicks;
extern int fake_sleeps;
extern int fake_kicks_stopped;
extern char fake_log[256];
extern void (*fake_stop_hook)(void);

void fake_logw(const char *w);

#endif
