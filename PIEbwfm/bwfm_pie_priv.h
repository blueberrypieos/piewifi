/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * PIEbwfm's own state, and what its glue needs from the platform:
 * bwfm_pie_xnu.c in the kext, the fakes in the host tests.
 */
#ifndef _BWFM_PIE_PRIV_H
#define _BWFM_PIE_PRIV_H

#include "bwfm_pie.h"
#include "bwfm_pie_os.h"
#include "bwfm_pie_kext.h"

#define BWFM_PIE_FWDIR		"/usr/share/firmware/brcm/"
#define BWFM_PIE_RXQ_MAX	256

struct pie80211_drv;
struct pie80211_fullmac_ops;
struct ieee80211com;
enum ieee80211_state;

/*
 * bus_off and reload are taken under the card's lock: the reset callback
 * sets them, outside the gate. While bus_off is set, the bus work and the
 * interrupt leave the card alone and firmware commands fail at once.
 * reload makes the next init set the chip up and load its firmware again,
 * through bus_reset.
 */
struct bwfm_pie {
	struct bwfm_softc *sc;
	struct pie_sdio_func *f1;
	struct sdmmc_function sf[3];
	struct rwlock lock;
	int (*intr_fn)(void *);
	void *intr_arg;
	struct device *mount_dev;
	void (*mount_fn)(struct device *);
	int attached;
	struct mbuf_list rxq;
	struct task rx_task;
	struct task start_task;
	struct pie80211_drv *drv;
	int node;
	struct bwfm_pie_os *os;
	void (*bus_attach)(struct device *, struct device *, void *);
	int (*bus_reset)(struct bwfm_softc *);
	void (*bus_clocks)(struct bwfm_softc *, int);
	int (*newstate)(struct ieee80211com *, enum ieee80211_state, int);
	unsigned int generation;
	unsigned int reset_seen;
	int reset_on;
	int bus_off;
	int reload;
	int reload_fails;
	unsigned int resets;
	uint8_t mac[6];
	int has_mac;
	char country[2];
	int has_country;
	uint64_t link_when;
	int link_error;
	int8_t link_rssi;
	uint32_t link_rate;
};

int bwfm_pie_attach(struct bwfm_pie *p);
struct pie80211_drv *bwfm_pie_drv_attach(struct bwfm_pie *p, int *error);
void bwfm_pie_detach(struct bwfm_pie *p);
int bwfm_pie_of_add(void *osnode);
void bwfm_pie_of_clear(void);
int bwfm_pie_firmware_allowed(const char *name, const u_char *buf, size_t len);
int bwfm_pie_set_country(void *softc, const char code[2]);
int bwfm_pie_nvram_macaddr(const u_char *buf, size_t len, uint8_t mac[6]);
int bwfm_pie_set_mac(struct bwfm_softc *sc, const uint8_t *shared);
int bwfm_pie_link_info(void *softc, int8_t *rssi, uint32_t *rate_kbps);
void bwfm_pie_card_reset(void *arg);
void bwfm_pie_reset_watch(struct bwfm_pie *p);
extern const struct pie80211_fullmac_ops bwfm_pie_fmops;

#endif
