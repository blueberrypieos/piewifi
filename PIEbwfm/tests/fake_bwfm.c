/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * Fakes for PIEbwfm's host tests: bwfm_pie_os.h without threads, a device
 * tree of two nodes, firmware files from a table, and <pie/sdio.h> calls
 * that are recorded. The lock reads its bwfm_pie_os, so a freed one fails
 * under the address sanitizer. fake_log gets a word for each stop, free,
 * reset callback change, interrupt removal, function disabled and pause of
 * 10 ms or more, and fake_stop_hook runs as the bus work stops, as the
 * last of the bus thread's work would.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pie/sdio.h>
#include "../bwfm_pie_os.h"
#include "fake_bwfm.h"

void *pie80211_malloc(size_t size, int type, int flags);

struct bwfm_pie_os {
	void (*bus_fn)(void *);
	void *bus_arg;
	int locks;
	int stopped;
};

struct fake_sdio fake_sdio;
struct fake_prop fake_props[FAKE_PROPS];
struct fake_file fake_files[FAKE_FILES];
int fake_reads;
char fake_read_path[FAKE_READS][128];
int fake_kicks;
int fake_sleeps;
int fake_kicks_stopped;
char fake_log[256];
void (*fake_stop_hook)(void);

void
fake_logw(const char *w)
{
	size_t n = strlen(fake_log);

	snprintf(fake_log + n, sizeof(fake_log) - n, "%s%s", n != 0 ? " " : "", w);
}

struct bwfm_pie_os *
bwfm_pie_os_alloc(void (*bus_fn)(void *), void *bus_arg)
{
	struct bwfm_pie_os *os = calloc(1, sizeof(*os));

	if (os != NULL) {
		os->bus_fn = bus_fn;
		os->bus_arg = bus_arg;
	}
	return os;
}

void
bwfm_pie_os_stop(struct bwfm_pie_os *os)
{
	if (os == NULL || os->stopped)
		return;
	if (fake_stop_hook != NULL)
		fake_stop_hook();
	os->stopped = 1;
	fake_logw("stop");
}

void
bwfm_pie_os_free(struct bwfm_pie_os *os)
{
	if (os == NULL)
		return;
	if (!os->stopped && fake_stop_hook != NULL)
		fake_stop_hook();
	fake_logw("free");
	free(os);
}

void
bwfm_pie_os_lock(struct bwfm_pie_os *os)
{
	os->locks++;
}

void
bwfm_pie_os_unlock(struct bwfm_pie_os *os)
{
	os->locks--;
}

int
bwfm_pie_os_sleep(struct bwfm_pie_os *os, void *chan, uint64_t nsec)
{
	fake_sleeps++;
	return 1;
}

void
bwfm_pie_os_wakeup(void *chan)
{
}

void
bwfm_pie_os_kick(struct bwfm_pie_os *os)
{
	fake_kicks++;
	if (os->stopped)
		fake_kicks_stopped++;
}

void
bwfm_pie_os_delay(unsigned int us)
{
	char w[32];

	if (us >= 10000) {
		snprintf(w, sizeof(w), "delay-%u", us);
		fake_logw(w);
	}
}

int
bwfm_pie_os_readfile(const char *path, unsigned char **bufp, size_t *lenp)
{
	int i;

	if (fake_reads < FAKE_READS)
		snprintf(fake_read_path[fake_reads], sizeof(fake_read_path[0]), "%s", path);
	fake_reads++;
	for (i = 0; i < FAKE_FILES; i++) {
		if (fake_files[i].path == NULL || strcmp(fake_files[i].path, path) != 0)
			continue;
		*bufp = pie80211_malloc(fake_files[i].len, 1, 0);
		if (*bufp == NULL)
			return ENOMEM;
		memcpy(*bufp, fake_files[i].data, fake_files[i].len);
		*lenp = fake_files[i].len;
		return 0;
	}
	return ENOENT;
}

int
bwfm_pie_os_getprop(void *osnode, const char *name, void *buf, int len)
{
	int i;

	for (i = 0; i < FAKE_PROPS; i++) {
		if (fake_props[i].node != osnode || fake_props[i].name == NULL ||
		    strcmp(fake_props[i].name, name) != 0)
			continue;
		if (buf != NULL && len > 0)
			memcpy(buf, fake_props[i].data, (size_t)len < fake_props[i].len ? (size_t)len : fake_props[i].len);
		return (int)fake_props[i].len;
	}
	return -1;
}

static void
record(int op, unsigned int fn, uint32_t addr, size_t len, bool incr)
{
	struct fake_sdio_call *c;

	if (fake_sdio.ncalls >= FAKE_CALLS)
		return;
	c = &fake_sdio.calls[fake_sdio.ncalls++];
	c->op = op;
	c->fn = fn;
	c->addr = addr;
	c->len = len;
	c->incr = incr;
}

static unsigned int
fnum(struct pie_sdio_func *f)
{
	return f == FAKE_F1 ? 1 : f == FAKE_F2 ? 2 : 99;
}

int
pie_sdio_claim(struct pie_sdio_func *f)
{
	fake_sdio.claims++;
	return 0;
}

void
pie_sdio_release(struct pie_sdio_func *f)
{
	unsigned int gen = fake_sdio.reset_at_release;

	fake_sdio.releases++;
	if (gen != 0 && fake_sdio.reset_handler != NULL) {
		fake_sdio.reset_at_release = 0;
		fake_sdio.generation = gen;
		fake_sdio.reset_handler(fake_sdio.reset_arg);
	}
}

int
pie_sdio_read1(struct pie_sdio_func *f, unsigned int fn, uint32_t addr, uint8_t *val)
{
	record(FAKE_READ1, fn, addr, 1, false);
	*val = fake_sdio.read1_value;
	return 0;
}

int
pie_sdio_write1(struct pie_sdio_func *f, unsigned int fn, uint32_t addr, uint8_t val)
{
	record(FAKE_WRITE1, fn, addr, 1, false);
	return 0;
}

int
pie_sdio_read4(struct pie_sdio_func *f, uint32_t addr, uint32_t *val)
{
	record(FAKE_READ4, fnum(f), addr, 4, true);
	*val = fake_sdio.read4_value;
	return 0;
}

int
pie_sdio_write4(struct pie_sdio_func *f, uint32_t addr, uint32_t val)
{
	record(FAKE_WRITE4, fnum(f), addr, 4, true);
	return 0;
}

int
pie_sdio_read(struct pie_sdio_func *f, uint32_t addr, void *buf, size_t len, bool incr)
{
	record(FAKE_READ, fnum(f), addr, len, incr);
	memset(buf, 0, len);
	return 0;
}

int
pie_sdio_write(struct pie_sdio_func *f, uint32_t addr, const void *buf, size_t len, bool incr)
{
	record(FAKE_WRITE, fnum(f), addr, len, incr);
	return 0;
}

int
pie_sdio_enable(struct pie_sdio_func *f)
{
	record(FAKE_ENABLE, fnum(f), 0, 0, false);
	return 0;
}

int
pie_sdio_disable(struct pie_sdio_func *f)
{
	record(FAKE_DISABLE, fnum(f), 0, 0, false);
	fake_logw(fnum(f) == 1 ? "disable-1" : fnum(f) == 2 ? "disable-2" : "disable-other");
	return 0;
}

int
pie_sdio_set_block_size(struct pie_sdio_func *f, unsigned int size)
{
	record(FAKE_BLOCK, fnum(f), 0, size, false);
	return 0;
}

int
pie_sdio_interrupt_register(struct pie_sdio_func *f, void (*handler)(void *), void *arg)
{
	record(FAKE_IRQ_ON, fnum(f), 0, 0, false);
	fake_sdio.handler = handler;
	fake_sdio.handler_arg = arg;
	return 0;
}

int
pie_sdio_interrupt_unregister(struct pie_sdio_func *f)
{
	record(FAKE_IRQ_OFF, fnum(f), 0, 0, false);
	fake_sdio.handler = NULL;
	fake_logw("irq-off");
	if (fake_sdio.set_up_on_unregister)
		fake_sdio.generation++;
	return 0;
}

int
pie_sdio_reset_card(struct pie_sdio_func *f)
{
	record(FAKE_RESET, fnum(f), 0, 0, false);
	if (fake_sdio.claims - fake_sdio.releases == 1)
		fake_sdio.resets_claimed++;
	fake_logw("card-reset");
	if (fake_sdio.reset_error == 0)
		fake_sdio.generation++;
	return fake_sdio.reset_error;
}

unsigned int
pie_sdio_generation(struct pie_sdio_func *f)
{
	return fake_sdio.generation;
}

int
pie_sdio_reset_register(struct pie_sdio_func *f, void (*handler)(void *), void *arg)
{
	fake_sdio.reset_handler = handler;
	fake_sdio.reset_arg = arg;
	fake_logw(handler != NULL ? "reset-on" : "reset-off");
	if (handler != NULL && fake_sdio.reset_at_register)
		handler(arg);
	return 0;
}

struct pie_sdio_func *
pie_sdio_function(struct pie_sdio_func *f, unsigned int fn)
{
	return fn == 1 ? FAKE_F1 : fn == 2 && !fake_sdio.no_f2 ? FAKE_F2 : NULL;
}
