/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pie/sdio.h>
#include <pie80211/driver.h>
#include <pie80211/family.h>
#include "regulatory/rtw88_regulatory.h"
#include "rtw88.h"
#include "rtw88_os.h"

#define P_START 0x01
#define P_STOP 0x02
#define P_STATE 0x04
#define P_KEY 0x08
#define P_DELETE_KEY 0x10
#define P_COUNTRY 0x20
#define P_EDCA 0x40
#define P_RESET 0x80
#define P_HOST_RESET 0x100
#define TX_QUEUE_SIZE 64
#define TX_QUEUE_HIGH 48
#define TX_QUEUE_LOW 16
#define TX_REPORT_SIZE 64
#define REPORT_TIMEOUT_NS UINT64_C(1000000000)
#define RECOVERY_WINDOW_NS UINT64_C(60000000000)
#define TX_STALL_NS UINT64_C(2000000000)

struct rtw88_queued {
	struct pie80211_mbuf *m;
	struct pie80211_drv_tx tx;
	uint64_t key_epoch;
};
struct rtw88_report {
	uint32_t cookie;
	uint64_t deadline;
};
struct rtw88_pie {
	struct rtw88_hw hw;
	struct pie_sdio_func *function;
	struct rtw88_os *os;
	struct pie80211_drv *driver;
	uint8_t *firmware;
	size_t firmware_size;
	unsigned int generation;
	unsigned int pending;
	unsigned int epoch;
	uint64_t key_epoch;
	uint64_t hardware_key_epoch;
	uint64_t hardware_key_generation;
	unsigned int tx_head;
	unsigned int tx_count;
	unsigned int report_next;
	unsigned int retries;
	uint64_t last_failure;
	uint64_t tx_since[5];
	uint32_t tx_idle[3];
	uint32_t tx_last[3];
	unsigned int host_generation;
	bool ready;
	bool closing;
	bool desired_up;
	bool up;
	bool paused;
	bool function_enabled;
	bool interrupt_registered;
	bool reset_registered;
	bool faulted;
	uint8_t data_rate;
	uint8_t basic_rate;
	char country[2];
	char hardware_country[2];
	char pending_country[2];
	struct pie80211_drv_state state;
	struct pie80211_drv_key key;
	struct pie80211_drv_edca edca[4];
	struct rtw88_queued queue[TX_QUEUE_SIZE];
	struct rtw88_report reports[TX_REPORT_SIZE];
	uint8_t frame[RTW88_MAX_FRAME];
};

static void worker(void *arg);

static void
wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;
	while (len-- != 0)
		*p++ = 0;
}

static int
read_byte(void *arg, uint32_t addr, uint8_t *value)
{
	int error = pie_sdio_read1(arg, 1, addr, value);
	if (error != 0)
		rtw88_os_failure("CMD52 read", addr, error);
	return error;
}

static int
write_byte(void *arg, uint32_t addr, uint8_t value)
{
	int error = pie_sdio_write1(arg, 1, addr, value);
	if (error != 0)
		rtw88_os_failure("CMD52 write", addr, error);
	return error;
}

static int
read_bytes(void *arg, uint32_t addr, void *buf, size_t len, bool incr)
{
	int error = pie_sdio_read(arg, addr, buf, len, incr);
	if (error != 0)
		rtw88_os_failure("CMD53 read", addr, error);
	return error;
}

static int
write_bytes(void *arg, uint32_t addr, const void *buf, size_t len, bool incr)
{
	int error = pie_sdio_write(arg, addr, buf, len, incr);
	if (error != 0)
		rtw88_os_failure("CMD53 write", addr, error);
	return error;
}

static void
delay(void *arg, unsigned int usec)
{
	(void)arg;
	rtw88_os_delay(usec);
}

static void
interrupt(void *arg)
{
	struct rtw88_pie *pie = arg;
	(void)rtw88_irq_mask(&pie->hw, 0);
	rtw88_os_kick(pie->os);
}

static void
reset(void *arg)
{
	struct rtw88_pie *pie = arg;
	rtw88_os_lock(pie->os);
	if (!pie->closing) {
		pie->pending |= P_HOST_RESET;
		pie->host_generation = pie_sdio_generation(pie->function);
	}
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static int
initialize(struct rtw88_pie *pie)
{
	struct pie_rtw_reg_channel channels[PIE80211_DRV_MAXCHAN];
	char hardware_country[2];
	size_t count;
	int error;
	pie->hw.error = 0;
	pie->hw.failed_register = 0;
	pie->hw.powered = false;
	pie->hw.firmware_ready = false;
	pie->hw.key_valid = false;
	rtw88_os_lock(pie->os);
	pie->hardware_key_epoch = 0;
	pie->hardware_key_generation = 0;
	rtw88_os_unlock(pie->os);
	pie->hw.rx_addr = 0;
	pie->hw.rx_stopped = false;
	pie->hw.rx_was_enabled = false;
	pie->report_next = 0;
	memcpy(pie->hw.country, pie->country, sizeof(pie->country));
	pie->hw.io.max_request = pie_sdio_max_request(pie->function);
	pie->hw.io.block_size = pie_sdio_block_size(pie->function);
	pie->hw.uhs = pie_sdio_is_uhs(pie->function);
	pie->generation = pie_sdio_generation(pie->function);
	if (pie->generation == 0 || pie->hw.io.max_request < 512)
		return ENXIO;
	error = rtw88_power(&pie->hw, true);
	if (error != 0)
		rtw88_os_failure("power", pie->hw.failed_register, error);
	if (error == 0) {
		error = rtw88_download_firmware(&pie->hw, pie->firmware, pie->firmware_size);
		if (error != 0)
			rtw88_os_failure("firmware", pie->hw.failed_register, error);
	}
	if (error == 0) {
		error = rtw88_read_efuse(&pie->hw);
		if (error != 0)
			rtw88_os_failure("efuse", pie->hw.failed_register, error);
	}
	if (error == 0) {
		error = rtw88_country_decode(pie->hw.efuse + 0xcb, hardware_country);
		if (error != 0)
			return error;
		if (hardware_country[0] != 0) {
			error = pie_rtw_reg_lookup(hardware_country, channels, PIE80211_DRV_MAXCHAN, &count);
			if (error != 0 || count == 0)
				return EOPNOTSUPP;
		}
		if (pie->ready && memcmp(hardware_country,pie->hardware_country,2) != 0)
			return EIO;
		if (!pie->ready)
			memcpy(pie->hardware_country,hardware_country,2);
	}
	if (error == 0)
		error = rtw88_mac_init(&pie->hw);
	if (error == 0)
		error = rtw88_phy_init(&pie->hw);
	if (error == 0)
		error = rtw88_key_clear(&pie->hw);
	if (error == 0)
		error = rtw88_tx_free(&pie->hw, pie->tx_idle);
	memcpy(pie->tx_last, pie->tx_idle, sizeof(pie->tx_last));
	memset(pie->hw.tx_used, 0, sizeof(pie->hw.tx_used));
	rtw88_write8(&pie->hw, 0x0522, 0xff);
	pie->hw.tx_paused = true;
	return error != 0 ? error : pie->hw.error;
}

static void
start(void *arg)
{
	struct rtw88_pie *pie = arg;
	rtw88_os_lock(pie->os);
	pie->desired_up = true;
	pie->pending |= P_START;
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static void
stop(void *arg)
{
	struct rtw88_pie *pie = arg;
	rtw88_os_lock(pie->os);
	pie->desired_up = false;
	pie->pending |= P_STOP;
	pie->paused = true;
	pie->epoch++;
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static void
set_state(void *arg, const struct pie80211_drv_state *state)
{
	struct rtw88_pie *pie = arg;
	rtw88_os_lock(pie->os);
	pie->state = *state;
	pie->epoch++;
	pie->pending |= P_STATE;
	pie->paused = true;
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static void
set_key(void *arg, const struct pie80211_drv_key *key)
{
	struct rtw88_pie *pie = arg;
	rtw88_os_lock(pie->os);
	pie->key = *key;
	pie->key_epoch++;
	pie->pending |= P_KEY;
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static void
delete_key(void *arg, const struct pie80211_drv_key *key)
{
	struct rtw88_pie *pie = arg;
	(void)key;
	rtw88_os_lock(pie->os);
	wipe(&pie->key, sizeof(pie->key));
	pie->key_epoch++;
	pie->pending &= ~P_KEY;
	pie->pending |= P_DELETE_KEY;
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static void
set_edca(void *arg, const struct pie80211_drv_edca edca[4])
{
	struct rtw88_pie *pie = arg;
	rtw88_os_lock(pie->os);
	memcpy(pie->edca, edca, sizeof(pie->edca));
	pie->pending |= P_EDCA;
	rtw88_os_unlock(pie->os);
	rtw88_os_kick(pie->os);
}

static int
transmit(void *arg, struct pie80211_mbuf *m, const struct pie80211_drv_tx *tx)
{
	struct rtw88_pie *pie = arg;
	unsigned int at;
	bool full;
	int error = 0;
	rtw88_os_lock(pie->os);
	if (pie->closing || pie->faulted || !pie->desired_up || pie->paused)
		error = ENETDOWN;
	else if (pie->tx_count == TX_QUEUE_SIZE)
		error = ENOBUFS;
	if (error == 0) {
		at = (pie->tx_head + pie->tx_count) % TX_QUEUE_SIZE;
		pie->queue[at].m = m;
		pie->queue[at].tx = *tx;
		pie->queue[at].key_epoch = pie->key_epoch;
		pie->tx_count++;
	}
	full = pie->tx_count >= TX_QUEUE_HIGH;
	rtw88_os_unlock(pie->os);
	if (error != 0) {
		if ((tx->flags & PIE80211_DRV_TX_STATUS) != 0 && tx->cookie != 0)
			pie80211_drv_tx_done(pie->driver, tx->cookie, 0);
		pie80211_m_freem(m);
	}
	if (full)
		pie80211_drv_tx_flow(pie->driver, 1);
	rtw88_os_kick(pie->os);
	return error;
}

static int
policy_channels(struct rtw88_pie *pie, const char country[2],
    struct pie_rtw_reg_channel channels[PIE80211_DRV_MAXCHAN], size_t *count)
{
	size_t at, kept = 0;
	int16_t board_limit;
	int error = pie_rtw_reg_lookup(country, channels, PIE80211_DRV_MAXCHAN, count);
	if (error != 0)
		return error;
	for (at = 0; at < *count; at++) {
		if ((channels[at].freq < 3000 && !pie->hw.board->band_2g.supported) ||
		    (channels[at].freq > 5000 && !pie->hw.board->band_5g.supported))
			continue;
		channels[kept++] = channels[at];
	}
	*count = kept;
	kept = 0;
	if (pie->hardware_country[0] != 0) {
		for (at = 0; at < *count; at++) {
			error = pie_rtw_reg_limit(pie->hardware_country, channels[at].freq, &board_limit);
			if (error == EINVAL)
				return EOPNOTSUPP;
			if (error != 0)
				continue;
			channels[kept] = channels[at];
			if (channels[kept].max_eirp_mbm > board_limit)
				channels[kept].max_eirp_mbm = board_limit;
			kept++;
		}
		*count = kept;
	}
	return *count != 0 ? 0 : EOPNOTSUPP;
}

static int
policy_limit(struct rtw88_pie *pie, uint16_t freq, int16_t *limit)
{
	int16_t board_limit;
	int error = pie_rtw_reg_limit(pie->country, freq, limit);
	if (error == 0 && pie->hardware_country[0] != 0) {
		error = pie_rtw_reg_limit(pie->hardware_country, freq, &board_limit);
		if (error == 0 && *limit > board_limit)
			*limit = board_limit;
	}
	return error;
}

static int
set_country(void *arg, const char country[2])
{
	struct rtw88_pie *pie = arg;
	struct pie_rtw_reg_channel channels[PIE80211_DRV_MAXCHAN];
	size_t count;
	int error = policy_channels(pie, country, channels, &count);
	if (error != 0)
		return error;
	if (count == 0)
		return EOPNOTSUPP;
	rtw88_os_lock(pie->os);
	if (pie->closing || pie->faulted) {
		rtw88_os_unlock(pie->os);
		return ENETDOWN;
	}
	memcpy(pie->pending_country, country, sizeof(pie->pending_country));
	pie->pending |= P_COUNTRY;
	pie->paused = true;
	pie->epoch++;
	rtw88_os_unlock(pie->os);
	pie80211_drv_tx_flow(pie->driver, 1);
	rtw88_os_kick(pie->os);
	return 0;
}

static const struct pie80211_drv_ops operations = {
	start, stop, set_state, set_key, delete_key, set_edca, transmit, set_country
};

static void
purge_transmit(struct rtw88_pie *pie)
{
	struct rtw88_queued queued;
	unsigned int i;
	for (;;) {
		rtw88_os_lock(pie->os);
		if (pie->tx_count == 0) {
			rtw88_os_unlock(pie->os);
			break;
		}
		queued = pie->queue[pie->tx_head];
		pie->tx_head = (pie->tx_head + 1) % TX_QUEUE_SIZE;
		pie->tx_count--;
		rtw88_os_unlock(pie->os);
		if (queued.tx.cookie != 0)
			pie80211_drv_tx_done(pie->driver, queued.tx.cookie, 0);
		pie80211_m_freem(queued.m);
	}
	for (i = 0; i < TX_REPORT_SIZE; i++) {
		if (pie->reports[i].cookie != 0)
			pie80211_drv_tx_done(pie->driver, pie->reports[i].cookie, 0);
		pie->reports[i].cookie = 0;
	}
}

static int
receive(void *arg, const struct rtw88_rx_packet *packet)
{
	struct rtw88_pie *pie = arg;
	struct pie80211_drv_rx rx;
	struct pie80211_mbuf *m;
	uint8_t sequence;
	bool acked;
	if (packet->c2h) {
		if (rtw88_tx_report(packet->data, packet->len, &sequence, &acked) == 0) {
			unsigned int slot = sequence >> 2;
			if (pie->reports[slot].cookie != 0) {
				pie80211_drv_tx_done(pie->driver, pie->reports[slot].cookie, acked);
				pie->reports[slot].cookie = 0;
			}
		}
		return 0;
	}
	if (!pie->up || (packet->decrypted && !pie->hw.key_valid))
		return 0;
	m = pie80211_m_frombuf(packet->data, packet->len, 0);
	if (m == NULL)
		return 0;
	memset(&rx, 0, sizeof(rx));
	rx.freq = packet->freq;
	rx.rssi = packet->rssi;
	rx.tstamp = packet->timestamp;
	if (packet->decrypted) {
		rx.flags = PIE80211_DRV_RX_DECRYPTED | PIE80211_DRV_RX_KEY_GENERATION;
		rx.key_generation = pie->hardware_key_generation;
	}
	pie80211_drv_input(pie->driver, m, &rx);
	return 0;
}

static int
send_queued(struct rtw88_pie *pie)
{
	struct rtw88_queued queued;
	struct rtw88_tx_packet tx;
	size_t len;
	unsigned int processed, slot = 0, scanned;
	int error;
	for (processed = 0; processed < 8; processed++) {
		rtw88_os_lock(pie->os);
		if (pie->tx_count == 0 || pie->pending != 0 || pie->paused || !pie->desired_up) {
			rtw88_os_unlock(pie->os);
			return 0;
		}
		queued = pie->queue[pie->tx_head];
		if ((queued.tx.flags & PIE80211_DRV_TX_HW_KEY) != 0 &&
		    (!pie->hw.key_valid || queued.key_epoch != pie->hardware_key_epoch)) {
			pie->tx_head = (pie->tx_head + 1) % TX_QUEUE_SIZE;
			pie->tx_count--;
			rtw88_os_unlock(pie->os);
			if ((queued.tx.flags & PIE80211_DRV_TX_STATUS) != 0 && queued.tx.cookie != 0)
				pie80211_drv_tx_done(pie->driver, queued.tx.cookie, 0);
			pie80211_m_freem(queued.m);
			continue;
		}
		rtw88_os_unlock(pie->os);
		len = pie80211_m_pktlen(queued.m);
		if (len > sizeof(pie->frame) || len < 24)
			return EMSGSIZE;
		if (pie80211_m_tobuf(queued.m, pie->frame, sizeof(pie->frame)) != (int)len)
			return EINVAL;
		memset(&tx, 0, sizeof(tx));
		tx.qsel = (queued.tx.flags & PIE80211_DRV_TX_MGMT) != 0 ? 18 : queued.tx.tid;
		tx.rate = (queued.tx.flags & (PIE80211_DRV_TX_MGMT | PIE80211_DRV_TX_STATUS)) != 0 ?
		    pie->basic_rate : pie->data_rate;
		tx.no_ack = (queued.tx.flags & PIE80211_DRV_TX_NO_ACK) != 0;
		tx.protection = pie->hw.protection && !tx.no_ack && (pie->frame[4] & 1) == 0 &&
		    (queued.tx.flags & PIE80211_DRV_TX_MGMT) == 0;
		tx.rts_rate = pie->hw.rts_rate;
		tx.hardware_key = (queued.tx.flags & PIE80211_DRV_TX_HW_KEY) != 0;
		if (tx.hardware_key && !pie->hw.key_valid)
			return EACCES;
		tx.report = (queued.tx.flags & PIE80211_DRV_TX_STATUS) != 0;
		if (tx.report) {
			for (scanned = 0; scanned < TX_REPORT_SIZE; scanned++) {
				slot = (pie->report_next + scanned) % TX_REPORT_SIZE;
				if (pie->reports[slot].cookie == 0)
					break;
			}
			if (scanned == TX_REPORT_SIZE)
				return EAGAIN;
			tx.report_id = (uint8_t)(slot << 2);
		}
		error = rtw88_tx_descriptor(pie->hw.scratch, sizeof(pie->hw.scratch), pie->frame, len, &tx);
		if (error != 0)
			return error;
		memcpy(pie->hw.scratch + RTW88_TXDESC, pie->frame, len);
		error = rtw88_write_port(&pie->hw, tx.qsel, pie->hw.scratch, len + RTW88_TXDESC, true);
		if (error != 0)
			return error;
		rtw88_os_lock(pie->os);
		pie->tx_head = (pie->tx_head + 1) % TX_QUEUE_SIZE;
		pie->tx_count--;
		rtw88_os_unlock(pie->os);
		if (tx.report) {
			pie->report_next = (slot + 1) % TX_REPORT_SIZE;
			pie->reports[slot].cookie = queued.tx.cookie;
			pie->reports[slot].deadline = rtw88_os_now() + REPORT_TIMEOUT_NS;
		}
		pie80211_m_freem(queued.m);
	}
	return 0;
}

static int32_t
pages_field(const uint32_t pages[3], unsigned int field)
{
	return (int32_t)((pages[field / 2] >> (16 * (field % 2))) & 0xfff);
}

static int
tx_watch(struct rtw88_pie *pie, bool reopened)
{
	uint32_t pages[3];
	uint64_t now = rtw88_os_now();
	int32_t current, expected;
	unsigned int field;
	bool rebase;
	int error = rtw88_tx_free(&pie->hw, pages), stalled = 0;
	if (error != 0)
		return error;
	rtw88_os_lock(pie->os);
	rebase = reopened || pie->hw.tx_paused || pie->paused ||
	    (pie->pending & (P_STOP | P_STATE | P_COUNTRY | P_RESET | P_HOST_RESET)) != 0;
	rtw88_os_unlock(pie->os);
	for (field = 0; field < 5; field++) {
		current = pages_field(pages, field);
		expected = pages_field(pie->tx_last, field) - (int32_t)pie->hw.tx_used[field];
		if (field != 3) {
			current += pages_field(pages, 3);
			expected += pages_field(pie->tx_last, 3) - (int32_t)pie->hw.tx_used[3];
		}
		if (rebase || current > expected ||
		    pages_field(pages, field) >= pages_field(pie->tx_idle, field))
			pie->tx_since[field] = now;
		else if (now - pie->tx_since[field] >= TX_STALL_NS)
			stalled = ETIMEDOUT;
	}
	memcpy(pie->tx_last, pages, sizeof(pie->tx_last));
	memset(pie->hw.tx_used, 0, sizeof(pie->hw.tx_used));
	return stalled;
}

static void
publish(struct rtw88_pie *pie, unsigned int epoch, uint64_t key_epoch,
    uint64_t key_generation, bool started,
    int start_error, bool state_done, bool key_done, int key_error)
{
	rtw88_os_lock(pie->os);
	if (epoch == pie->epoch && !pie->closing) {
		if (started)
			pie80211_drv_started(pie->driver, start_error);
		if (state_done)
			pie80211_drv_state_done(pie->driver);
		if (key_done && key_epoch == pie->key_epoch)
			pie80211_drv_key_done_generation(pie->driver, key_generation, key_error);
	}
	rtw88_os_unlock(pie->os);
}

static void
worker(void *arg)
{
	struct rtw88_pie *pie = arg;
	struct pie80211_drv_state state;
	struct pie80211_drv_key key;
	struct pie80211_drv_edca edca[4];
	struct pie_rtw_reg_channel allowed[PIE80211_DRV_MAXCHAN];
	struct pie80211_drv_chan channels[PIE80211_DRV_MAXCHAN];
	unsigned int flags, epoch, at, host_generation;
	uint64_t key_epoch, key_generation;
	uint16_t response_rates = 0;
	bool desired, closing, state_done = false, key_done = false, start_done = false;
	char country[2];
	int error = 0, key_error = 0, irq_error;
	const char *stage = "bus claim";
	int16_t limit;
	size_t count = 0;
	uint64_t now;
	rtw88_os_lock(pie->os);
	if (!pie->ready || pie->closing) {
		rtw88_os_unlock(pie->os);
		return;
	}
	flags = pie->pending;
	pie->pending &= P_COUNTRY;
	epoch = pie->epoch;
	key_epoch = pie->key_epoch;
	host_generation = pie->host_generation;
	desired = pie->desired_up;
	state = pie->state;
	key = pie->key;
	key_generation = key.generation;
	if ((flags & P_KEY) != 0)
		wipe(&pie->key, sizeof(pie->key));
	memcpy(edca, pie->edca, sizeof(edca));
	memcpy(country, pie->pending_country, sizeof(country));
	if ((flags & P_COUNTRY) != 0)
		memcpy(pie->country, country, sizeof(country));
	rtw88_os_unlock(pie->os);
	if (pie->faulted) {
		wipe(&key, sizeof(key));
		if ((flags & P_START) != 0 && desired)
			publish(pie, epoch, key_epoch, key_generation, true, ENXIO, false, false, 0);
		return;
	}
	if ((flags & P_STATE) != 0)
		purge_transmit(pie);
	error = pie_sdio_claim(pie->function);
	if (error != 0)
		goto failed;
	if ((flags & P_HOST_RESET) != 0 && (host_generation == 0 || host_generation > pie->generation))
		flags |= P_RESET;
	if (pie_sdio_generation(pie->function) != pie->generation || pie->hw.error != 0) {
		if ((flags & P_RESET) == 0) {
			rtw88_os_lock(pie->os);
			pie->last_failure = rtw88_os_now();
			rtw88_os_unlock(pie->os);
		}
		flags |= P_RESET;
	}
	if ((flags & P_RESET) != 0) {
		stage = "card reset";
		rtw88_os_lock(pie->os);
		if ((flags & P_HOST_RESET) != 0)
			pie->last_failure = rtw88_os_now();
		pie->paused = true;
		pie->epoch++;
		rtw88_os_unlock(pie->os);
		pie->up = false;
		if (pie->retries >= 3) {
			error = EIO;
			pie_sdio_release(pie->function);
			goto failed;
		}
		pie->retries++;
		if (pie_sdio_generation(pie->function) == pie->generation)
			error = pie_sdio_reset_card(pie->function);
		if (error == 0)
			error = initialize(pie);
		pie_sdio_release(pie->function);
		purge_transmit(pie);
		wipe(&key, sizeof(key));
		if (error != 0)
			goto failed;
		pie80211_drv_restart(pie->driver);
		return;
	}
	if ((flags & P_COUNTRY) != 0) {
		stage = "country";
		error = rtw88_tx_quiesce(&pie->hw);
		if (error == 0)
			error = rtw88_rx_quiesce(&pie->hw, receive, pie);
		irq_error = rtw88_irq_mask(&pie->hw, 0);
		if (error == 0)
			error = irq_error;
		if (error == 0)
			error = rtw88_key_clear(&pie->hw);
		pie->up = false;
		if (error == 0)
			error = policy_channels(pie, country, allowed, &count);
		if (error == 0 && count != 0) {
			memcpy(pie->hw.country, country, sizeof(country));
			error = rtw88_phy_channel(&pie->hw, allowed[0].freq, allowed[0].max_eirp_mbm);
			for (at = 0; at < count; at++) {
				channels[at].freq = allowed[at].freq;
				channels[at].flags = allowed[at].freq < 3000 ? PIE80211_DRV_CHAN_2GHZ : PIE80211_DRV_CHAN_5GHZ;
			}
		}
		pie_sdio_release(pie->function);
		purge_transmit(pie);
		wipe(&key, sizeof(key));
		if (error == 0 && count != 0)
			error = pie80211_drv_channels(pie->driver, channels, count);
		if (error != 0)
			goto failed;
		rtw88_os_lock(pie->os);
		if (memcmp(country, pie->pending_country, sizeof(country)) == 0)
			pie->pending &= ~P_COUNTRY;
		pie->pending &= ~P_STATE;
		rtw88_os_unlock(pie->os);
		return;
	}
	if ((flags & P_STOP) != 0) {
		stage = "stop";
		pie->up = false;
		error = rtw88_tx_quiesce(&pie->hw);
		if (error == 0)
			error = rtw88_rx_quiesce(&pie->hw, receive, pie);
		irq_error = rtw88_irq_mask(&pie->hw, 0);
		if (error == 0)
			error = irq_error;
		if (error == 0)
			error = rtw88_key_clear(&pie->hw);
	}
	if (error == 0 && (flags & P_START) != 0 && desired) {
		stage = "start";
		error = policy_channels(pie, pie->country, allowed, &count);
		if (error == 0 && count == 0)
			error = EOPNOTSUPP;
		if (error == 0)
			error = rtw88_phy_channel(&pie->hw, allowed[0].freq, allowed[0].max_eirp_mbm);
		if (error == 0) {
			rtw88_os_lock(pie->os);
			if ((pie->pending & P_COUNTRY) == 0) {
				pie->up = true;
				start_done = true;
				pie->data_rate = pie->basic_rate = allowed[0].freq > 5000 ? 4 : 0;
			}
			rtw88_os_unlock(pie->os);
		}
	}
	if (error == 0 && (flags & P_STATE) != 0 && pie->up) {
		stage = "state";
		error = rtw88_tx_quiesce(&pie->hw);
		if (error == 0)
			error = rtw88_rx_quiesce(&pie->hw, receive, pie);
		if (error == 0 && state.state != PIE80211_DRV_S_INIT) {
			error = policy_limit(pie, state.freq, &limit);
			if (error == 0)
				error = rtw88_phy_channel(&pie->hw, state.freq, limit);
			if (error == 0 && state.state == PIE80211_DRV_S_AUTH)
				error = rtw88_calibrate(&pie->hw);
		}
		if (error == 0) {
			static const uint8_t legacy[] = {2,4,11,22,12,18,24,36,48,72,96,108};
			unsigned int i, j;
			for (i = 0; i < state.nrates && i < PIE80211_DRV_MAXRATES; i++)
				if ((state.rates[i] & 0x80) != 0)
					for (j = 0; j < sizeof(legacy); j++)
						if ((state.rates[i] & 0x7f) == legacy[j])
							response_rates |= (uint16_t)(1U << j);
			if (state.state <= PIE80211_DRV_S_SCAN)
				pie->basic_rate = pie->hw.freq > 5000 ? 4 : 0;
			if (state.state >= PIE80211_DRV_S_AUTH && state.nrates != 0)
				error = rtw88_basic_rate(pie->hw.freq, state.rates, state.nrates, &pie->basic_rate);
			if (state.state == PIE80211_DRV_S_RUN && error == 0)
				error = rtw88_basic_rate(pie->hw.freq, state.rates, state.nrates, &pie->data_rate);
			if (error == 0)
				error = rtw88_state(&pie->hw, state.state, state.bssid, state.aid,
			    state.beacon_int, state.erp, response_rates);
		}
		state_done = error == 0;
		if (state_done && state.state != PIE80211_DRV_S_INIT)
			error = rtw88_rx_resume(&pie->hw);
	}
	if (error == 0 && (flags & (P_KEY | P_DELETE_KEY)) != 0) {
		stage = "key";
		error = rtw88_tx_quiesce(&pie->hw);
		if (error == 0)
			error = rtw88_rx_quiesce(&pie->hw, receive, pie);
	}
	if (error == 0 && (flags & P_DELETE_KEY) != 0) {
		error = rtw88_key_clear(&pie->hw);
		rtw88_os_lock(pie->os);
		pie->hardware_key_epoch = 0;
		pie->hardware_key_generation = 0;
		rtw88_os_unlock(pie->os);
	}
	if (error == 0 && (flags & P_KEY) != 0 && pie->up) {
		if (key.len == 16)
			key_error = rtw88_key(&pie->hw, key.addr, key.key);
		else {
			error = rtw88_key_clear(&pie->hw);
			key_error = EINVAL;
		}
		rtw88_os_lock(pie->os);
		pie->hardware_key_epoch = key_error == 0 ? key_epoch : 0;
		pie->hardware_key_generation = key_error == 0 ? key_generation : 0;
		rtw88_os_unlock(pie->os);
		key_done = true;
	}
	if (error == 0 && (flags & (P_KEY | P_DELETE_KEY)) != 0 && pie->up) {
		error = rtw88_rx_resume(&pie->hw);
		if (error == 0) {
			rtw88_write8(&pie->hw, 0x0522, 0);
			pie->hw.tx_paused = false;
		}
	}
	if (error == 0 && (flags & P_EDCA) != 0 && pie->up) {
		stage = "EDCA";
		for (at = 0; at < 4 && error == 0; at++)
			error = rtw88_edca(&pie->hw, at, edca[at].aifsn, edca[at].ecwmin, edca[at].ecwmax, edca[at].txop);
	}
	rtw88_os_lock(pie->os);
	if (pie->up && (pie->pending & (P_STOP | P_STATE | P_COUNTRY | P_RESET | P_HOST_RESET)) == 0)
		pie->paused = false;
	closing = pie->closing;
	rtw88_os_unlock(pie->os);
	if (error == 0 && pie->up && !closing) {
		stage = "receive";
		error = rtw88_rx_drain(&pie->hw, receive, pie);
	}
	if (error == 0) {
		now = rtw88_os_now();
		for (at = 0; at < TX_REPORT_SIZE; at++) {
			if (pie->reports[at].cookie != 0 && now >= pie->reports[at].deadline) {
				pie80211_drv_tx_done(pie->driver, pie->reports[at].cookie, 0);
				pie->reports[at].cookie = 0;
			}
		}
	}
	if (error == 0 && pie->up && !closing) {
		stage = "transmit";
		error = send_queued(pie);
		if (error == EAGAIN)
			error = 0;
	}
	if (error == 0 && pie->up && !closing) {
		stage = "transmit queue";
		error = tx_watch(pie, (flags & (P_START | P_STATE | P_KEY | P_DELETE_KEY)) != 0);
	}
	if (pie->up && error == 0) {
		stage = "interrupt mask";
		error = rtw88_irq_mask(&pie->hw, RTW88_IRQ_MASK);
	}
	pie_sdio_release(pie->function);
	wipe(&key, sizeof(key));
	if (error != 0 || pie->hw.error != 0)
		goto failed;
	publish(pie, epoch, key_epoch, key_generation, start_done, 0, state_done, key_done, key_error);
	if ((flags & P_STOP) != 0)
		purge_transmit(pie);
	rtw88_os_lock(pie->os);
	now = rtw88_os_now();
	if (pie->up && now - pie->last_failure >= RECOVERY_WINDOW_NS)
		pie->retries = 0;
	desired = pie->tx_count < TX_QUEUE_LOW && !pie->paused;
	rtw88_os_unlock(pie->os);
	if (desired)
		pie80211_drv_tx_flow(pie->driver, 0);
	return;
failed:
	wipe(&key, sizeof(key));
	pie->up = false;
	pie->hw.key_valid = false;
	pie->hw.firmware_ready = false;
	rtw88_os_lock(pie->os);
	pie->last_failure = rtw88_os_now();
	pie->paused = true;
	if (pie->retries < 3)
		pie->pending |= P_RESET;
	else
		pie->faulted = true;
	rtw88_os_unlock(pie->os);
	if (pie->faulted && pie_sdio_claim(pie->function) == 0) {
		(void)rtw88_irq_mask(&pie->hw, 0);
		if (pie->interrupt_registered) {
			(void)pie_sdio_interrupt_unregister(pie->function);
			pie->interrupt_registered = false;
		}
		pie_sdio_release(pie->function);
	}
	purge_transmit(pie);
	rtw88_os_error(stage, pie->hw.failed_register,
	    error != 0 ? error : pie->hw.error != 0 ? pie->hw.error : EIO, (int)pie->retries);
	if ((flags & P_START) != 0 && desired)
		publish(pie, epoch, key_epoch, key_generation, true, error != 0 ? error : EIO, false, false, 0);
	if (pie->faulted)
		pie80211_drv_restart(pie->driver);
	else
		pie80211_drv_lost(pie->driver);
}

struct rtw88_pie *
rtw88_pie_new(struct pie_sdio_func *function, const struct rtw88_board_profile *profile)
{
	struct rtw88_pie *pie = rtw88_os_alloc_mem(sizeof(*pie));
	if (pie == NULL)
		return NULL;
	pie->function = function;
	pie->hw.board = profile;
	pie->os = rtw88_os_new(worker, pie);
	if (pie->os == NULL) {
		rtw88_os_free_mem(pie, sizeof(*pie));
		return NULL;
	}
	pie->country[0] = pie->country[1] = '0';
	pie->pending_country[0] = pie->pending_country[1] = '0';
	pie->hw.country[0] = pie->hw.country[1] = '0';
	pie->hw.io = (struct rtw88_io){read_byte, write_byte, read_bytes, write_bytes, delay, function, 0, 512};
	return pie;
}

struct pie80211_drv *
rtw88_pie_attach(struct rtw88_pie *pie, int *error)
{
	struct pie80211_drv_desc description;
	struct pie_rtw_reg_channel allowed[PIE80211_DRV_MAXCHAN];
	size_t count, at;
	*error = rtw88_board_validate(pie->hw.board);
	if (*error != 0)
		return NULL;
	*error = rtw88_os_firmware(&pie->firmware, &pie->firmware_size);
	if (*error != 0)
		return NULL;
	*error = pie_sdio_claim(pie->function);
	if (*error != 0)
		return NULL;
	if (pie_sdio_vendor(pie->function) != RTW88_SDIO_VENDOR ||
	    pie_sdio_device(pie->function) != RTW88_SDIO_DEVICE ||
	    pie_sdio_number(pie->function) != RTW88_SDIO_FUNCTION) {
		pie_sdio_release(pie->function);
		*error = ENODEV;
		return NULL;
	}
	*error = pie_sdio_set_block_size(pie->function, 512);
	if (*error != 0)
		rtw88_os_failure("block size", 0, *error);
	if (*error == 0) {
		*error = pie_sdio_enable(pie->function);
		if (*error != 0) {
			rtw88_os_failure("function enable", 0, *error);
			(void)pie_sdio_disable(pie->function);
		}
		pie->function_enabled = *error == 0;
	}
	if (*error == 0)
		*error = initialize(pie);
	pie_sdio_release(pie->function);
	if (*error != 0)
		return NULL;
	*error = policy_channels(pie, pie->country, allowed, &count);
	if (*error != 0 || count == 0) {
		if (*error == 0)
			*error = EOPNOTSUPP;
		return NULL;
	}
	memset(&description, 0, sizeof(description));
	memcpy(description.addr, pie->hw.mac, sizeof(description.addr));
	description.caps = PIE80211_DRV_CAP_SHORT_PREAMBLE | PIE80211_DRV_CAP_SHORT_SLOT;
	description.nchan = (uint32_t)count;
	for (at = 0; at < count; at++) {
		description.chan[at].freq = allowed[at].freq;
		description.chan[at].flags = allowed[at].freq < 3000 ? PIE80211_DRV_CHAN_2GHZ : PIE80211_DRV_CHAN_5GHZ;
	}
	pie->driver = pie80211_drv_attach(&description, &operations, pie, error);
	if (pie->driver == NULL)
		return NULL;
	*error = pie_sdio_reset_register(pie->function, reset, pie);
	if (*error != 0)
		return NULL;
	pie->reset_registered = true;
	*error = pie_sdio_claim(pie->function);
	if (*error != 0)
		return NULL;
	*error = pie_sdio_interrupt_register(pie->function, interrupt, pie);
	pie_sdio_release(pie->function);
	if (*error != 0)
		return NULL;
	pie->interrupt_registered = true;
	rtw88_os_lock(pie->os);
	pie->ready = true;
	rtw88_os_unlock(pie->os);
	return pie->driver;
}

void
rtw88_pie_delete(struct rtw88_pie *pie)
{
	if (pie == NULL)
		return;
	rtw88_os_lock(pie->os);
	pie->closing = true;
	pie->ready = false;
	rtw88_os_unlock(pie->os);
	if (pie->reset_registered)
		(void)pie_sdio_reset_register(pie->function, NULL, NULL);
	rtw88_os_stop(pie->os);
	if (pie->interrupt_registered && pie_sdio_claim(pie->function) == 0) {
		(void)pie_sdio_interrupt_unregister(pie->function);
		pie_sdio_release(pie->function);
	}
	if (pie->function_enabled && pie_sdio_claim(pie->function) == 0) {
		if (pie->hw.error == 0) {
			(void)rtw88_key_clear(&pie->hw);
			(void)rtw88_power(&pie->hw, false);
		}
		(void)pie_sdio_disable(pie->function);
		pie_sdio_release(pie->function);
	}
	purge_transmit(pie);
	if (pie->driver != NULL)
		pie80211_drv_detach(pie->driver);
	if (pie->firmware != NULL)
		rtw88_os_free_mem(pie->firmware, pie->firmware_size);
	rtw88_os_free(pie->os);
	wipe(pie, sizeof(*pie));
	rtw88_os_free_mem(pie, sizeof(*pie));
}
