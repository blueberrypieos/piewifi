/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * The platform under PIEbwfm's glue, in plain C types: bwfm_pie_xnu.c and
 * BWFMSDIO.cpp in the kext, fakes in the host tests. One lock and one bus
 * thread for each card. The bus thread runs bus_fn(bus_arg) once for each
 * series of kicks. Once stop has returned, bus_fn does not run and is not
 * running, and kicks do nothing; free stops it too, and frees the lock.
 * sleep drops the lock while it waits, and returns nonzero if nsec passed
 * first. readfile allocates its buffer with the
 * family's malloc, so free(buf, M_DEVBUF, len) releases it. getprop copies
 * at most len bytes of a device tree node's property and returns its
 * length, or -1 if the node has none.
 */
#ifndef _BWFM_PIE_OS_H
#define _BWFM_PIE_OS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bwfm_pie_os;

struct bwfm_pie_os *bwfm_pie_os_alloc(void (*bus_fn)(void *), void *bus_arg);
void bwfm_pie_os_stop(struct bwfm_pie_os *os);
void bwfm_pie_os_free(struct bwfm_pie_os *os);
void bwfm_pie_os_lock(struct bwfm_pie_os *os);
void bwfm_pie_os_unlock(struct bwfm_pie_os *os);
int bwfm_pie_os_sleep(struct bwfm_pie_os *os, void *chan, uint64_t nsec);
void bwfm_pie_os_wakeup(void *chan);
void bwfm_pie_os_kick(struct bwfm_pie_os *os);
void bwfm_pie_os_delay(unsigned int us);
int bwfm_pie_os_readfile(const char *path, unsigned char **bufp, size_t *lenp);
int bwfm_pie_os_getprop(void *osnode, const char *name, void *buf, int len);

#ifdef __cplusplus
}
#endif

#endif
