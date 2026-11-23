/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * The calls BWFMSDIO.cpp makes into PIEbwfm's C glue. root and node are
 * the device tree's root and the WiFi chip's node, as IORegistryEntry
 * pointers that stay valid until bwfm_pie_delete(). bwfm_pie_attach()
 * loads the firmware and gives the driver handle for attachDriver(), or
 * NULL with *error set.
 *
 * And the worker BWFMSDIO.cpp gives bwfm_pie_xnu.c for the bus work: an
 * event source on a work loop of its own. Each kick runs fn(arg) on the
 * loop's thread, once for a series of kicks. Once stop has returned, fn
 * does not run and is not running, as the loop's gate guarantees, and no
 * kick may follow. free stops it too.
 */
#ifndef _BWFM_PIE_KEXT_H
#define _BWFM_PIE_KEXT_H

#ifdef __cplusplus
extern "C" {
#endif

struct bwfm_pie;
struct pie_sdio_func;
struct pie80211_drv;

struct bwfm_pie_worker;

struct bwfm_pie *bwfm_pie_new(struct pie_sdio_func *f1, void *root, void *node);
struct pie80211_drv *bwfm_pie_start(struct bwfm_pie *p, int *error);
void bwfm_pie_delete(struct bwfm_pie *p);

struct bwfm_pie_worker *bwfm_pie_worker_new(void (*fn)(void *), void *arg);
void bwfm_pie_worker_kick(struct bwfm_pie_worker *w);
void bwfm_pie_worker_stop(struct bwfm_pie_worker *w);
void bwfm_pie_worker_free(struct bwfm_pie_worker *w);

#ifdef __cplusplus
}
#endif

#endif
