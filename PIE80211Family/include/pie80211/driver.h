/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * The interface between PIE80211Family and an 802.11 driver.
 *
 * The family calls the driver's operations in its gate. They must not
 * sleep: the driver queues the work, and reports the end of start,
 * set_state and set_key with pie80211_drv_started(),
 * pie80211_drv_state_done() and pie80211_drv_key_done(). The driver calls
 * the functions from pie80211_drv_attach() to pie80211_drv_restart() from
 * any thread outside the gate, never from a primary interrupt or filter,
 * and none of them after pie80211_drv_detach(). Attach and detach enter
 * the gate themselves. pie80211_drv_tx_done() and pie80211_drv_tx_flow()
 * may also be called by tx and set_country in the family gate.
 *
 * The controller calls pie80211_drv_set_host(), _up(), _down(), _output(),
 * _ioctl() and _tx_stopped() in the gate. pie80211_drv_cmdinfo() gives a
 * command's data length, whether it changes state, and whether it needs
 * root: every command that changes state does, and so does the list of
 * stored networks.
 *
 * The family sends EAPOL frames with PIE80211_DRV_TX_STATUS and a cookie.
 * The driver reports each of them once with pie80211_drv_tx_done(), as
 * acknowledged or not, also when it drops one. The driver calls
 * pie80211_drv_tx_flow() when its queue fills and when it drains. While it
 * is full the controller holds its output, and the host's tx_wake restarts
 * it. Management and EAPOL frames still reach the driver then.
 *
 * Only pairwise CCMP keys reach the driver. A frame sent with
 * PIE80211_DRV_TX_HW_KEY carries its CCMP header with the packet number,
 * and the driver adds the MIC. A frame the driver decrypted keeps its CCMP
 * header and MIC, and is marked PIE80211_DRV_RX_DECRYPTED.
 */
#ifndef _PIE80211_DRIVER_H
#define _PIE80211_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pie80211_mbuf;
struct pie80211_drv;

#define PIE80211_DRV_MAXCHAN		64
#define PIE80211_DRV_MAXRATES		15

#define PIE80211_DRV_CHAN_2GHZ		0x0001
#define PIE80211_DRV_CHAN_5GHZ		0x0002
#define PIE80211_DRV_CHAN_PASSIVE	0x0004

#define PIE80211_DRV_CAP_SHORT_PREAMBLE	0x0001
#define PIE80211_DRV_CAP_SHORT_SLOT	0x0002
#define PIE80211_DRV_CAP_HT		0x0004
#define PIE80211_DRV_CAP_MFP		0x0008

struct pie80211_drv_chan {
	uint16_t freq;
	uint16_t flags;
};

struct pie80211_drv_desc {
	uint8_t addr[6];
	uint32_t caps;
	uint32_t nchan;
	struct pie80211_drv_chan chan[PIE80211_DRV_MAXCHAN];
	uint16_t ht_cap;
	uint8_t ht_ampdu;
	uint8_t ht_mcs[10];
};

#define PIE80211_DRV_S_INIT		0
#define PIE80211_DRV_S_SCAN		1
#define PIE80211_DRV_S_AUTH		2
#define PIE80211_DRV_S_ASSOC		3
#define PIE80211_DRV_S_RUN		4

#define PIE80211_DRV_ERP_SHORT_SLOT	0x01
#define PIE80211_DRV_ERP_SHORT_PREAMBLE	0x02
#define PIE80211_DRV_ERP_PROTECTION	0x04

struct pie80211_drv_state {
	int state;
	int prev;
	uint16_t freq;
	uint8_t bssid[6];
	uint16_t aid;
	uint16_t beacon_int;
	uint8_t dtim_period;
	uint8_t erp;
	uint8_t nrates;
	uint8_t rates[PIE80211_DRV_MAXRATES];
	uint8_t ht;
	uint16_t ht_cap;
	uint8_t ht_ampdu;
	uint8_t ht_mcs[10];
};

struct pie80211_drv_key {
	uint8_t addr[6];
	uint8_t len;
	uint8_t key[16];
	uint64_t rsc;
	uint64_t tsc;
	uint64_t generation;
};

struct pie80211_drv_edca {
	uint8_t aifsn;
	uint8_t ecwmin;
	uint8_t ecwmax;
	uint8_t acm;
	uint16_t txop;
};

#define PIE80211_DRV_TX_NO_ACK		0x0001
#define PIE80211_DRV_TX_STATUS		0x0002
#define PIE80211_DRV_TX_HW_KEY		0x0004
#define PIE80211_DRV_TX_MGMT		0x0008

struct pie80211_drv_tx {
	uint32_t flags;
	uint32_t cookie;
	uint8_t ac;
	uint8_t tid;
};

#define PIE80211_DRV_RX_DECRYPTED	0x0001
#define PIE80211_DRV_RX_KEY_GENERATION	0x0002

struct pie80211_drv_rx {
	uint32_t flags;
	uint16_t freq;
	int8_t rssi;
	uint32_t tstamp;
	uint64_t key_generation;
};

struct pie80211_drv_ops {
	void (*start)(void *softc);
	void (*stop)(void *softc);
	void (*set_state)(void *softc, const struct pie80211_drv_state *st);
	void (*set_key)(void *softc, const struct pie80211_drv_key *key);
	void (*delete_key)(void *softc, const struct pie80211_drv_key *key);
	void (*set_edca)(void *softc, const struct pie80211_drv_edca edca[4]);
	int (*tx)(void *softc, struct pie80211_mbuf *m, const struct pie80211_drv_tx *tx);
	int (*set_country)(void *softc, const char code[2]);
};

struct pie80211_drv *pie80211_drv_attach(const struct pie80211_drv_desc *desc,
    const struct pie80211_drv_ops *ops, void *softc, int *error);
void pie80211_drv_detach(struct pie80211_drv *d);
/* usage: quiesce driver callbacks, then call outside the family gate. */
int pie80211_drv_channels(struct pie80211_drv *d, const struct pie80211_drv_chan *channels,
    size_t nchan);
void pie80211_drv_started(struct pie80211_drv *d, int error);
void pie80211_drv_state_done(struct pie80211_drv *d);
void pie80211_drv_key_done(struct pie80211_drv *d, int error);
/* usage: complete the generation supplied to set_key. */
void pie80211_drv_key_done_generation(struct pie80211_drv *d, uint64_t generation, int error);
void pie80211_drv_input(struct pie80211_drv *d, struct pie80211_mbuf *m,
    const struct pie80211_drv_rx *rx);
void pie80211_drv_tx_done(struct pie80211_drv *d, uint32_t cookie, int acked);
void pie80211_drv_lost(struct pie80211_drv *d);
void pie80211_drv_restart(struct pie80211_drv *d);
void pie80211_drv_tx_flow(struct pie80211_drv *d, int full);

struct pie80211_drv_host {
	void (*input)(void *ctx, struct pie80211_mbuf *m);
	void (*link)(void *ctx, int up, uint64_t baudrate);
	void (*restart)(void *ctx);
	void (*tx_wake)(void *ctx);
	void *ctx;
};

void pie80211_drv_set_host(struct pie80211_drv *d, const struct pie80211_drv_host *host);
int pie80211_drv_up(struct pie80211_drv *d);
void pie80211_drv_down(struct pie80211_drv *d);
int pie80211_drv_output(struct pie80211_drv *d, struct pie80211_mbuf *m);
int pie80211_drv_ioctl(struct pie80211_drv *d, uint32_t cmd, void *data, size_t len);
int pie80211_drv_cmdinfo(uint32_t cmd, size_t *len, int *set, int *root);
int pie80211_drv_tx_stopped(struct pie80211_drv *d);
const uint8_t *pie80211_drv_addr(struct pie80211_drv *d);

#ifdef __cplusplus
}
#endif

#endif
