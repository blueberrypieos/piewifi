/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rtw88_pie.c"

struct rtw88_os {
	void (*fn)(void *);
	void *arg;
	bool locked;
	bool stopped;
};
struct pie_sdio_func {
	unsigned int generation;
	unsigned int number;
	uint16_t vendor, device;
	bool claimed;
	bool enabled;
	int enable_error;
	uint32_t irq_mask;
	void (*interrupt)(void *);
	void *interrupt_arg;
	void (*reset)(void *);
	void *reset_arg;
};
struct pie80211_drv {
	const struct pie80211_drv_ops *ops;
	void *softc;
};
struct pie80211_mbuf {
	size_t len;
	uint8_t data[64];
};
static const struct rtw88_board_profile synthetic_board = {
	.supported_rfe_mask = 0x55,
	.band_2g = {true,300,2300},
	.band_5g = {true,500,1700}
};

static bool gate;
static unsigned int starts, states, keys, inputs, restarts, countries, frees, reports, errors;
static unsigned int hardware_calls, country_applied, packets_sent;
static unsigned int allocation_count;
static unsigned int mac_calls, phy_calls;
static uint8_t efuse_country[2];
static int fake_error;
static int receive_error;
static int port_error;
static int channels_error;
static uint8_t last_rate;
static uint8_t last_report_id;
static uint16_t forbidden_freq;
static size_t family_channel_count;
static struct pie80211_drv_chan family_channels[PIE80211_DRV_MAXCHAN];
static uint64_t received_key_generation;
static int last_start_error;
static uint64_t fake_key_generation, completed_key_generation;
static bool defer_key_completion;
static bool family_running;
static void (*receive_hook)(void);
static uint64_t now_ns;
static void (*key_hook)(void);
static void (*channel_hook)(struct rtw88_hw *);
static void (*channels_hook)(void);
static bool public_pages;
static struct pie80211_drv fake_driver;
static struct rtw88_os *last_os;

static void
hardware(void)
{
	assert(!gate);
	hardware_calls++;
}

void *rtw88_os_alloc_mem(size_t size)
{
	allocation_count++;
	return calloc(1, size);
}
void rtw88_os_free_mem(void *p, size_t size)
{
	(void)size;
	assert(allocation_count > 0);
	allocation_count--;
	free(p);
}
struct rtw88_os *rtw88_os_new(void (*fn)(void *), void *arg)
{
	struct rtw88_os *os = calloc(1, sizeof(*os));
	os->fn = fn;
	os->arg = arg;
	last_os = os;
	return os;
}
void rtw88_os_free(struct rtw88_os *os)
{
	assert(os->stopped && !os->locked);
	free(os);
	last_os = NULL;
}
void rtw88_os_lock(struct rtw88_os *os)
{
	assert(!os->locked);
	os->locked = true;
}
void rtw88_os_unlock(struct rtw88_os *os)
{
	assert(os->locked);
	os->locked = false;
}
void rtw88_os_kick(struct rtw88_os *os) { assert(os != NULL); }
void rtw88_os_stop(struct rtw88_os *os)
{
	assert(!os->locked);
	os->fn(os->arg);
	os->stopped = true;
}
void rtw88_os_delay(unsigned int usec) { now_ns += (uint64_t)usec * 1000; }
uint64_t rtw88_os_now(void) { return now_ns; }
void rtw88_os_error(const char *stage, uint32_t address, int error, int retry)
{
	assert(stage != NULL && error != 0 && retry >= 0 && retry <= 3);
	(void)address;
	errors++;
}
void rtw88_os_failure(const char *stage, uint32_t address, int error)
{
	assert(stage != NULL && error != 0);
	(void)address;
}
int rtw88_os_firmware(uint8_t **data, size_t *size)
{
	*size = 64;
	*data = rtw88_os_alloc_mem(*size);
	return *data != NULL ? 0 : ENOMEM;
}

int pie_sdio_claim(struct pie_sdio_func *f)
{
	assert(!f->claimed);
	f->claimed = true;
	return 0;
}
void pie_sdio_release(struct pie_sdio_func *f)
{
	assert(f->claimed);
	f->claimed = false;
}
int pie_sdio_read1(struct pie_sdio_func *f, unsigned int fn, uint32_t addr, uint8_t *val)
{
	(void)addr;
	assert(f->claimed && fn == 1);
	*val = 0;
	return 0;
}
int pie_sdio_write1(struct pie_sdio_func *f, unsigned int fn, uint32_t addr, uint8_t val)
{
	(void)addr; (void)val;
	assert(f->claimed && fn == 1);
	return 0;
}
int pie_sdio_read(struct pie_sdio_func *f, uint32_t addr, void *buf, size_t len, bool incr)
{
	(void)addr; (void)incr;
	assert(f->claimed);
	memset(buf, 0, len);
	return 0;
}
int pie_sdio_write(struct pie_sdio_func *f, uint32_t addr, const void *buf, size_t len, bool incr)
{
	(void)addr; (void)buf; (void)len; (void)incr;
	assert(f->claimed);
	return 0;
}
int pie_sdio_set_block_size(struct pie_sdio_func *f, unsigned int size)
{
	assert(f->claimed && size == 512);
	return 0;
}
int pie_sdio_enable(struct pie_sdio_func *f) { assert(f->claimed); f->enabled = true; return f->enable_error; }
int pie_sdio_disable(struct pie_sdio_func *f) { assert(f->claimed); f->enabled = false; return 0; }
size_t pie_sdio_max_request(struct pie_sdio_func *f) { assert(f->claimed); return 65536; }
unsigned int pie_sdio_block_size(struct pie_sdio_func *f) { assert(f->claimed); return 512; }
bool pie_sdio_is_uhs(struct pie_sdio_func *f) { assert(f->claimed); return false; }
unsigned int pie_sdio_generation(struct pie_sdio_func *f) { return f->generation; }
unsigned int pie_sdio_number(struct pie_sdio_func *f) { assert(f->claimed); return f->number; }
uint16_t pie_sdio_vendor(struct pie_sdio_func *f) { assert(f->claimed); return f->vendor; }
uint16_t pie_sdio_device(struct pie_sdio_func *f) { assert(f->claimed); return f->device; }
int pie_sdio_reset_register(struct pie_sdio_func *f, void (*fn)(void *), void *arg)
{
	assert(!f->claimed);
	f->reset = fn;
	f->reset_arg = arg;
	return 0;
}
int pie_sdio_reset_card(struct pie_sdio_func *f)
{
	assert(f->claimed);
	f->generation++;
	return 0;
}
int pie_sdio_interrupt_register(struct pie_sdio_func *f, void (*fn)(void *), void *arg)
{
	assert(f->claimed);
	f->interrupt = fn;
	f->interrupt_arg = arg;
	return 0;
}
static bool unregister_after_stop;
int pie_sdio_interrupt_unregister(struct pie_sdio_func *f)
{
	assert(f->claimed);
	unregister_after_stop = last_os != NULL && last_os->stopped;
	f->interrupt = NULL;
	f->interrupt_arg = NULL;
	return 0;
}

int rtw88_power(struct rtw88_hw *hw, bool on) { hardware(); hw->powered = on; return fake_error; }
int rtw88_download_firmware(struct rtw88_hw *hw, const uint8_t *buf, size_t len)
{
	(void)buf; (void)len;
	hardware(); hw->firmware_ready = fake_error == 0; return fake_error;
}
int rtw88_read_efuse(struct rtw88_hw *hw)
{
	static const uint8_t addr[6] = {0x02,1,2,3,4,5};
	hardware(); memcpy(hw->mac, addr, sizeof(addr)); memcpy(hw->efuse + 0xcb,efuse_country,2); return fake_error;
}
int rtw88_mac_init(struct rtw88_hw *hw) { (void)hw; hardware(); mac_calls++; return fake_error; }
int rtw88_phy_init(struct rtw88_hw *hw) { (void)hw; hardware(); phy_calls++; return fake_error; }
static const struct rtw88_rx_packet *quiesce_packet;
int rtw88_rx_quiesce(struct rtw88_hw *hw, int (*fn)(void *, const struct rtw88_rx_packet *), void *arg)
{
	hardware(); hw->rx_stopped = true;
	if (quiesce_packet != NULL)
		assert(fn(arg,quiesce_packet) == 0);
	return fake_error;
}
int rtw88_rx_resume(struct rtw88_hw *hw) { hardware(); hw->rx_stopped = false; return fake_error; }
int rtw88_tx_quiesce(struct rtw88_hw *hw) { (void)hw; hardware(); return fake_error; }
int rtw88_key_clear(struct rtw88_hw *hw) { hardware(); hw->key_valid = false; return fake_error; }
int rtw88_key(struct rtw88_hw *hw, const uint8_t addr[6], const uint8_t key[16])
{
	(void)addr; (void)key; hardware(); hw->key_valid = fake_error == 0; if (key_hook != NULL) key_hook(); return fake_error;
}
void rtw88_write8(struct rtw88_hw *hw, uint32_t addr, uint8_t value)
{ (void)hw; (void)addr; (void)value; hardware(); }
int rtw88_irq_mask(struct rtw88_hw *hw, uint32_t mask)
{
	struct pie_sdio_func *f = hw->io.ctx;
	assert(f->claimed);
	hardware(); f->irq_mask = mask; return fake_error;
}
int rtw88_phy_channel(struct rtw88_hw *hw, uint16_t freq, int16_t limit)
{
	assert(limit > 0 && !last_os->locked && freq != forbidden_freq);
	hardware(); hw->freq = freq; country_applied++;
	if (channel_hook != NULL)
		channel_hook(hw);
	return fake_error;
}
int rtw88_calibrate(struct rtw88_hw *hw) { (void)hw; hardware(); return fake_error; }
int rtw88_state(struct rtw88_hw *hw, int state, const uint8_t addr[6], uint16_t aid, uint16_t beacon, uint8_t erp, uint16_t response_rates)
{ (void)hw; (void)state; (void)addr; (void)aid; (void)beacon; (void)erp; (void)response_rates; hardware(); return fake_error; }
int rtw88_edca(struct rtw88_hw *hw, unsigned int ac, uint8_t aifsn, uint8_t cwmin, uint8_t cwmax, uint16_t txop)
{ (void)hw; (void)ac; (void)aifsn; (void)cwmin; (void)cwmax; (void)txop; hardware(); return fake_error; }
int rtw88_rx_drain(struct rtw88_hw *hw, int (*fn)(void *, const struct rtw88_rx_packet *), void *arg)
{ (void)hw; (void)fn; (void)arg; hardware(); if (receive_hook != NULL) receive_hook(); return receive_error; }
int rtw88_tx_descriptor(uint8_t *dst, size_t cap, const uint8_t *frame, size_t len, const struct rtw88_tx_packet *tx)
{
	(void)frame; (void)len;
	assert(cap >= 48 && tx->rate <= 11);
	last_rate = tx->rate;
	if (tx->report)
		last_report_id = tx->report_id;
	memset(dst, 0, 48);
	return 0;
}
static uint32_t fake_pages[3] = {0x00100010,0x018d0010,0x0020000e};
int rtw88_tx_free(struct rtw88_hw *hw, uint32_t pages[3])
{
	(void)hw; hardware(); memcpy(pages,fake_pages,sizeof(fake_pages)); return fake_error;
}
int rtw88_write_port(struct rtw88_hw *hw, uint8_t qsel, const void *buf, size_t len, bool check)
{
	(void)qsel; (void)buf; (void)len;
	assert(hw->freq != forbidden_freq);
	assert(check); hardware(); if (port_error != 0) return port_error; packets_sent++;
	hw->tx_used[public_pages ? 3 : qsel == 18 ? 4 : 2]++;
	return fake_error;
}
int rtw88_tx_report(const uint8_t *data, size_t len, uint8_t *seq, bool *acked)
{
	if (len != 2)
		return ENOENT;
	*seq = data[0]; *acked = data[1] != 0; return 0;
}

int pie_rtw_reg_lookup(const char country[2], struct pie_rtw_reg_channel *channels, size_t cap, size_t *count)
{
	if (memcmp(country,"XX",2) == 0) { *count = 0; return 0; }
	if (memcmp(country,"00",2) != 0 && memcmp(country,"AU",2) != 0 && memcmp(country,"GB",2) != 0)
		return EINVAL;
	assert(cap >= 2);
	channels[0] = (struct pie_rtw_reg_channel){2412,2000};
	channels[1] = (struct pie_rtw_reg_channel){5745,2000};
	*count = memcmp(country,"GB",2) == 0 ? 1 : 2;
	return 0;
}
int pie_rtw_reg_limit(const char country[2], uint16_t freq, int16_t *limit)
{
	if (memcmp(country,"US",2) == 0 || memcmp(country,"GB",2) == 0) {
		if (freq != 2412)
			return EOPNOTSUPP;
		*limit = 1000;
		return 0;
	}
	if (memcmp(country,"00",2) != 0 && memcmp(country,"AU",2) != 0)
		return EINVAL;
	if (freq != 2412 && freq != 5745)
		return EOPNOTSUPP;
	*limit = 2000;
	return 0;
}
struct pie80211_drv *pie80211_drv_attach(const struct pie80211_drv_desc *desc,
    const struct pie80211_drv_ops *ops, void *softc, int *error)
{
	assert(desc->nchan == 2 && (desc->caps & PIE80211_DRV_CAP_HT) == 0);
	fake_driver = (struct pie80211_drv){ops,softc};
	*error = 0;
	return &fake_driver;
}
void pie80211_drv_detach(struct pie80211_drv *d) { assert(d == &fake_driver && last_os->stopped); }
void pie80211_drv_started(struct pie80211_drv *d, int error) { assert(d == &fake_driver && last_os->locked); starts++; last_start_error = error; family_running = error == 0; }
void pie80211_drv_state_done(struct pie80211_drv *d) { assert(d == &fake_driver && last_os->locked); states++; }
void pie80211_drv_key_done_generation(struct pie80211_drv *d, uint64_t generation, int error)
{ assert(d == &fake_driver && error == 0 && last_os->locked); completed_key_generation = generation; }
void pie80211_drv_tx_flow(struct pie80211_drv *d, int full) { assert(d == &fake_driver); (void)full; }
void pie80211_drv_lost(struct pie80211_drv *d) { assert(d == &fake_driver); }
void pie80211_drv_restart(struct pie80211_drv *d)
{
	assert(d == &fake_driver);
	restarts++;
	family_running = false;
	gate = true; d->ops->stop(d->softc); d->ops->start(d->softc); gate = false;
}
int pie80211_drv_channels(struct pie80211_drv *d, const struct pie80211_drv_chan *channels, size_t count)
{
	struct rtw88_pie *pie = d->softc;
	assert(!gate && !last_os->locked && !pie->function->claimed);
	assert((pie->pending & P_COUNTRY) != 0 && !pie->up && pie->paused);
	assert(count >= 1 && count <= 2 && channels[0].freq == 2412 && country_applied != 0);
	if (channels_hook != NULL)
		channels_hook();
	if (channels_error != 0)
		return channels_error;
	family_channel_count = count;
	memcpy(family_channels,channels,count * sizeof(*channels));
	countries++;
	gate = true; d->ops->stop(d->softc); d->ops->start(d->softc); gate = false;
	return 0;
}
void pie80211_drv_tx_done(struct pie80211_drv *d, uint32_t cookie, int acked)
{ assert(d == &fake_driver && cookie != 0); reports++; (void)acked; }
void pie80211_drv_input(struct pie80211_drv *d, struct pie80211_mbuf *m, const struct pie80211_drv_rx *rx)
{ assert(d == &fake_driver && rx->freq == 2412); if ((rx->flags & PIE80211_DRV_RX_DECRYPTED) != 0) { assert((rx->flags & PIE80211_DRV_RX_KEY_GENERATION) != 0); received_key_generation = rx->key_generation; } inputs++; pie80211_m_freem(m); }
struct pie80211_mbuf *pie80211_m_frombuf(const void *buf, size_t len, size_t lead)
{
	struct pie80211_mbuf *m;
	(void)lead;
	assert(len <= 64);
	m = malloc(sizeof(*m)); m->len = len; memcpy(m->data,buf,len); return m;
}
size_t pie80211_m_pktlen(struct pie80211_mbuf *m) { return m->len; }
int pie80211_m_tobuf(struct pie80211_mbuf *m, void *buf, size_t cap)
{ if (cap < m->len) return -1; memcpy(buf,m->data,m->len); return (int)m->len; }
struct pie80211_mbuf *pie80211_m_freem(struct pie80211_mbuf *m) { frees++; free(m); return NULL; }

static void
consume_key(void)
{
	if (completed_key_generation != 0 && completed_key_generation == fake_key_generation)
		keys++;
	completed_key_generation = 0;
}

static void
request_key(struct pie80211_drv *driver, const struct pie80211_drv_key *key)
{
	struct pie80211_drv_key request = *key;
	request.generation = ++fake_key_generation;
	driver->ops->set_key(driver->softc,&request);
}

static void
pump(struct rtw88_pie *pie)
{
	assert(!gate);
	pie->os->fn(pie->os->arg);
	assert(!pie->os->locked && !pie->function->claimed);
	if (!defer_key_completion)
		consume_key();
	now_ns += 100000000;
}

static void
race_country(struct rtw88_hw *hw)
{
	(void)hw;
	channel_hook = NULL;
	gate = true;
	assert(fake_driver.ops->set_country(fake_driver.softc,"AU") == 0);
	gate = false;
}

static void
race_key(void)
{
	struct pie80211_drv_key key = {.len = 16};
	key_hook = NULL;
	gate = true;
	request_key(&fake_driver,&key);
	gate = false;
}

static void
race_stop(void)
{
	receive_hook = NULL;
	gate = true;
	fake_driver.ops->stop(fake_driver.softc);
	gate = false;
}

static void
test_enable_timeout(void)
{
	struct pie_sdio_func function = {.generation = 1, .number = 1,
	    .vendor = 0x024c, .device = 0xc821, .enable_error = ETIMEDOUT};
	struct rtw88_pie *pie = rtw88_pie_new(&function,&synthetic_board);
	int error;
	assert(pie != NULL);
	assert(rtw88_pie_attach(pie,&error) == NULL && error == ETIMEDOUT);
	assert(!function.enabled && !function.claimed && !pie->function_enabled);
	rtw88_pie_delete(pie);
	assert(!function.enabled && allocation_count == 0);
}

static void
test_report_slots(struct rtw88_pie *pie, struct pie80211_drv *driver)
{
	struct pie80211_drv_tx tx = {.flags = PIE80211_DRV_TX_STATUS};
	uint8_t frame[32] = {0x08,0x01}, report[2] = {0,1};
	struct rtw88_rx_packet packet = {.data = report, .len = 2, .c2h = true};
	unsigned int sent = packets_sent, done = reports, restarted = restarts;
	unsigned int generation = pie->function->generation, first, hole;
	for (unsigned int i = 0; i < 200; i++) {
		tx.cookie = 100 + i;
		assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
		pump(pie);
		assert(packets_sent == sent + i + 1 && pie->tx_count == 0);
		assert(pie->reports[last_report_id >> 2].cookie == tx.cookie);
		report[0] = last_report_id;
		assert(receive(pie,&packet) == 0 && reports == done + i + 1);
	}
	first = pie->report_next;
	for (unsigned int i = 0; i < TX_REPORT_SIZE; i++) {
		tx.cookie = 300 + i;
		assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	}
	for (unsigned int i = 0; i < TX_REPORT_SIZE / 8; i++)
		pump(pie);
	assert(pie->tx_count == 0 && pie->report_next == first);
	for (unsigned int i = 0; i < TX_REPORT_SIZE; i++)
		assert(pie->reports[(first + i) % TX_REPORT_SIZE].cookie == 300 + i);
	hole = (first + 7) % TX_REPORT_SIZE;
	report[0] = (uint8_t)(hole << 2);
	assert(receive(pie,&packet) == 0 && pie->reports[hole].cookie == 0);
	tx.cookie = 400;
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	assert(pie_sdio_claim(pie->function) == 0);
	assert(send_queued(pie) == 0);
	pie_sdio_release(pie->function);
	assert(last_report_id == (hole << 2) && pie->reports[hole].cookie == 400);
	tx.cookie = 401;
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	assert(pie_sdio_claim(pie->function) == 0);
	assert(send_queued(pie) == EAGAIN);
	pie_sdio_release(pie->function);
	pump(pie);
	assert(pie->tx_count == 1 && pie->pending == 0 && pie->up && !pie->paused);
	assert(packets_sent == sent + 200 + TX_REPORT_SIZE + 1);
	now_ns = pie->reports[first].deadline;
	pump(pie);
	assert(pie->tx_count == 0 && pie->reports[first].cookie == 401);
	assert(packets_sent == sent + 200 + TX_REPORT_SIZE + 2);
	assert(pie->function->generation == generation && restarts == restarted && pie->retries == 0);
	purge_transmit(pie);
}

static void
race_channels_country(void)
{
	channels_hook = NULL;
	race_country(NULL);
	forbidden_freq = 0;
}

static void
test_country_recovery(struct rtw88_pie *pie, struct pie80211_drv *driver)
{
	struct pie80211_drv_state scan = {.state = PIE80211_DRV_S_SCAN, .freq = 5745};
	struct pie80211_drv_tx tx = {0};
	uint8_t frame[32] = {0x08,0x01};
	unsigned int sent = packets_sent, applied = countries;
	assert(memcmp(pie->country,"AU",2) == 0);
	gate = true; driver->ops->set_state(pie,&scan); gate = false;
	pump(pie);
	assert(pie->hw.freq == 5745);
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	gate = true; assert(driver->ops->set_country(pie,"GB") == 0); gate = false;
	forbidden_freq = 5745;
	fake_error = ETIMEDOUT;
	pump(pie);
	assert(memcmp(pie->country,"GB",2) == 0 && !pie->up && pie->paused);
	assert((pie->pending & (P_COUNTRY | P_RESET)) == (P_COUNTRY | P_RESET));
	assert(countries == applied && packets_sent == sent);
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == ENETDOWN);
	fake_error = 0;
	gate = true; driver->ops->start(pie); gate = false;
	pump(pie);
	assert(!pie->up && (pie->pending & P_COUNTRY) != 0 && packets_sent == sent);
	assert(memcmp(pie->hw.country,"GB",2) == 0);
	pump(pie);
	assert(!pie->up && (pie->pending & P_COUNTRY) == 0 && countries == applied + 1);
	assert(family_channel_count == 1 && family_channels[0].freq == 2412);
	pump(pie);
	assert(pie->up && !pie->paused && pie->hw.freq == 2412);
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	pump(pie);
	assert(packets_sent == sent + 1);
	gate = true; assert(driver->ops->set_country(pie,"GB") == 0); gate = false;
	channels_error = EIO;
	pump(pie);
	assert(!pie->up && (pie->pending & P_COUNTRY) != 0 && countries == applied + 1);
	channels_error = 0;
	pump(pie);
	assert(!pie->up && (pie->pending & P_COUNTRY) != 0);
	channels_hook = race_channels_country;
	pump(pie);
	assert((pie->pending & P_COUNTRY) != 0 && memcmp(pie->pending_country,"AU",2) == 0);
	assert(family_channel_count == 1 && !pie->up);
	pump(pie);
	assert((pie->pending & P_COUNTRY) == 0 && memcmp(pie->country,"AU",2) == 0);
	assert(family_channel_count == 2 && family_channels[1].freq == 5745);
	pump(pie);
	assert(pie->up && !pie->paused);
}

static void
test_interrupt_mask(struct rtw88_pie *pie)
{
	struct pie_sdio_func *function = pie->function;
	pie->hw.error = ETIMEDOUT;
	pie->hw.failed_register = 0x40;
	function->irq_mask = RTW88_IRQ_MASK;
	assert(pie_sdio_claim(function) == 0);
	function->interrupt(function->interrupt_arg);
	pie_sdio_release(function);
	assert(function->irq_mask == 0 && pie->hw.error == ETIMEDOUT && pie->hw.failed_register == 0x40);
	pie->hw.error = 0;
	pie->hw.failed_register = 0;
}

static void
test_tx_stall(struct rtw88_pie *pie)
{
	unsigned int generation = pie->function->generation, retries = pie->retries;
	assert(pie->up && memcmp(pie->tx_idle,fake_pages,sizeof(fake_pages)) == 0);
	pie->hw.tx_paused = false;
	fake_pages[2] = 0x0020000c;
	for (unsigned int i = 0; i < 30; i++) {
		fake_pages[2] = i % 2 == 0 ? 0x0020000c : 0x0020000d;
		pump(pie);
	}
	assert(pie->up && pie->function->generation == generation);
	pie->hw.tx_paused = true;
	fake_pages[2] = 0x0020000b;
	for (unsigned int i = 0; i < 30; i++)
		pump(pie);
	assert(pie->up && pie->function->generation == generation);
	pie->hw.tx_paused = false;
	fake_pages[2] = 0x0020000a;
	for (unsigned int i = 0; i < 30; i++) {
		uint8_t probe[24] = {0x40};
		struct pie80211_drv_tx management = {.flags = PIE80211_DRV_TX_MGMT};
		assert(fake_driver.ops->tx(pie,pie80211_m_frombuf(probe,sizeof(probe),0),&management) == 0);
		pump(pie);
	}
	assert(pie->up && pie->function->generation == generation);
	fake_pages[1] = 0x018d000f;
	pie->hw.tx_paused = true;
	pump(pie);
	pie->hw.tx_paused = false;
	for (unsigned int i = 0; i < 19; i++)
		pump(pie);
	assert(pie->up && pie->function->generation == generation);
	pump(pie);
	assert(!pie->up && (pie->pending & P_RESET) != 0);
	fake_pages[2] = 0x0020000e;
	pump(pie);
	assert(pie->function->generation > generation && pie->retries == retries + 1);
	pump(pie);
	assert(pie->up && pie->hw.tx_paused);
	fake_pages[1] = 0x018d0010;
	now_ns += RECOVERY_WINDOW_NS;
	pump(pie);
	assert(pie->up && pie->retries == 0);
	generation = pie->function->generation;
	pie->function->reset(pie->function->reset_arg);
	pump(pie);
	pump(pie);
	assert(pie->up && pie->retries == 0 && pie->function->generation == generation);
	pie->function->generation++;
	pie->function->reset(pie->function->reset_arg);
	pump(pie);
	assert(!pie->up && pie->retries == 1 && pie->generation == pie->function->generation);
	pump(pie);
	assert(pie->up);
	now_ns += RECOVERY_WINDOW_NS;
	pump(pie);
	assert(pie->retries == 0);
}

static void
stale_state(void)
{
	struct pie80211_drv_state auth = {.state = PIE80211_DRV_S_AUTH, .freq = 5745};
	channels_hook = NULL;
	fake_driver.ops->set_state(fake_driver.softc,&auth);
}

static void
test_tx_watch_edges(struct rtw88_pie *pie)
{
	unsigned int generation = pie->function->generation, retries = pie->retries;
	uint8_t probe[24] = {0x40};
	struct pie80211_drv_tx management = {.flags = PIE80211_DRV_TX_MGMT};
	struct pie80211_drv_edca edca[4] = {{2,4,10,0,0},{7,4,10,0,0},{1,3,4,0,94},{1,2,3,0,47}};
	assert(pie->up);
	pie->hw.tx_paused = false;
	public_pages = true;
	fake_pages[2] = 0x0020000a;
	for (unsigned int i = 0; i < 30; i++) {
		assert(fake_driver.ops->tx(pie,pie80211_m_frombuf(probe,sizeof(probe),0),&management) == 0);
		pump(pie);
	}
	public_pages = false;
	assert(pie->up && pie->function->generation == generation);
	fake_pages[2] = 0x001f000e;
	for (unsigned int i = 0; i < 30; i++)
		pump(pie);
	assert(pie->up && pie->function->generation == generation);
	fake_pages[2] = 0x0020000a;
	for (unsigned int i = 0; i < 19; i++) {
		gate = true; fake_driver.ops->set_edca(pie,edca); gate = false;
		pump(pie);
	}
	assert(pie->up && pie->function->generation == generation);
	gate = true; fake_driver.ops->set_edca(pie,edca); gate = false;
	pump(pie);
	assert(!pie->up && (pie->pending & P_RESET) != 0);
	fake_pages[2] = 0x0020000e;
	pump(pie);
	pump(pie);
	assert(pie->up && pie->retries == retries + 1);
	now_ns += RECOVERY_WINDOW_NS;
	pump(pie);
	assert(pie->retries == 0);
	generation = pie->function->generation;
	channels_hook = stale_state;
	gate = true; assert(fake_driver.ops->set_country(pie,"AU") == 0); gate = false;
	pump(pie);
	assert((pie->pending & (P_STATE | P_COUNTRY)) == 0 && channels_hook == NULL);
	pump(pie);
	assert(pie->up && pie->function->generation == generation && pie->retries == 0);
}

static void
test_recovery_budget(struct rtw88_pie *pie, struct pie80211_drv *driver)
{
	unsigned int retries = pie->retries, failed = errors;
	uint64_t failure;
	assert(pie->up && retries != 0);
	now_ns = pie->last_failure + RECOVERY_WINDOW_NS - UINT64_C(100000000);
	pump(pie);
	assert(pie->retries == retries);
	pump(pie);
	assert(pie->retries == 0);
	receive_error = EIO;
	pump(pie);
	failure = pie->last_failure;
	receive_error = 0;
	pump(pie);
	pump(pie);
	assert(pie->up && pie->retries == 1 && !pie->faulted && pie->last_failure == failure);
	gate = true; driver->ops->stop(pie); gate = false;
	pump(pie);
	now_ns = pie->last_failure + RECOVERY_WINDOW_NS;
	pump(pie);
	assert(!pie->up && pie->retries == 1);
	gate = true; driver->ops->start(pie); gate = false;
	pump(pie);
	assert(pie->up && pie->retries == 0);
	for (unsigned int i = 0; i < 4; i++) {
		receive_error = EIO;
		if (i == 3)
			pie->hw.error = EIO;
		pump(pie);
		receive_error = 0;
		if (i < 3) {
			assert(!pie->faulted && (pie->pending & P_RESET) != 0);
			pump(pie);
			pump(pie);
			assert(pie->up && pie->retries == i + 1);
		}
	}
	assert(pie->faulted && errors == failed + 5 && !family_running);
	assert(pie->function->irq_mask == 0 && pie->function->interrupt == NULL && !pie->interrupt_registered);
}

int
main(void)
{
	struct pie_sdio_func function = {.generation = 1, .number = 1, .vendor = 0x024c, .device = 0xc821};
	struct rtw88_pie *pie;
	struct pie80211_drv *driver;
	struct pie80211_drv_state state = {.state = PIE80211_DRV_S_AUTH, .freq = 2412};
	struct pie80211_drv_key key = {.len = 16};
	struct pie80211_drv_tx tx = {.cookie = 7, .flags = PIE80211_DRV_TX_STATUS};
	uint8_t frame[32] = {0x08,0x01};
	uint8_t zero[sizeof(key)] = {0};
	unsigned int before;
	int error;
	test_enable_timeout();
	{
		struct pie_sdio_func other = {.generation = 1, .number = 1, .vendor = 0x024c, .device = 0xc821};
		struct rtw88_pie *detached = rtw88_pie_new(&other,&synthetic_board);
		assert(detached != NULL && rtw88_pie_attach(detached,&error) != NULL);
		assert(other.interrupt != NULL);
		rtw88_pie_delete(detached);
		assert(other.interrupt == NULL && unregister_after_stop && allocation_count == 0);
	}
	pie = rtw88_pie_new(&function,&synthetic_board);
	assert(pie != NULL);
	{
		struct rtw88_pie *missing = rtw88_pie_new(&function,NULL);
		unsigned int calls = hardware_calls;
		assert(missing != NULL);
		assert(rtw88_pie_attach(missing,&error) == NULL && error == EOPNOTSUPP);
		rtw88_pie_delete(missing);
		assert(hardware_calls == calls && !function.enabled);
		last_os = pie->os;
	}
	{
		struct pie_sdio_func wrong = {.generation = 1, .number = 1, .vendor = 0x024c, .device = 0xc822};
		struct rtw88_pie *rejected = rtw88_pie_new(&wrong,&synthetic_board);
		unsigned int calls = hardware_calls;
		assert(rejected != NULL);
		assert(rtw88_pie_attach(rejected,&error) == NULL && error == ENODEV);
		rtw88_pie_delete(rejected);
		assert(hardware_calls == calls && !wrong.enabled);
		last_os = pie->os;
	}
	{
		unsigned int mac_before = mac_calls, phy_before = phy_calls;
		assert(pie_sdio_claim(&function) == 0);
		efuse_country[0] = 'A'; efuse_country[1] = 0xff;
		assert(initialize(pie) == EILSEQ && mac_calls == mac_before && phy_calls == phy_before);
		efuse_country[0] = efuse_country[1] = 'Z';
		assert(initialize(pie) == EOPNOTSUPP && mac_calls == mac_before && phy_calls == phy_before);
		efuse_country[0] = 'A'; efuse_country[1] = 'U';
		pie->ready = true;
		assert(initialize(pie) == EIO && mac_calls == mac_before && phy_calls == phy_before);
		pie->ready = false;
		efuse_country[0] = efuse_country[1] = 0xff;
		pie_sdio_release(&function);
	}
	driver = rtw88_pie_attach(pie,&error);
	assert(driver != NULL && error == 0 && function.interrupt != NULL && function.reset != NULL);
	{
		struct pie_rtw_reg_channel channels[PIE80211_DRV_MAXCHAN];
		size_t count;
		assert(pie->hardware_country[0] == 0);
		assert(policy_channels(pie,"AU",channels,&count) == 0 && count == 2 && channels[1].freq == 5745);
		memcpy(pie->hardware_country,"US",2);
		assert(policy_channels(pie,"AU",channels,&count) == 0 && count == 1 && channels[0].max_eirp_mbm == 1000);
		memcpy(pie->hardware_country,"ZZ",2);
		assert(policy_channels(pie,"AU",channels,&count) == EOPNOTSUPP);
		memset(pie->hardware_country,0,2);
	}
	gate = true;
	driver->ops->start(pie);
	gate = false;
	pump(pie);
	assert(starts == 1 && pie->up && !pie->paused);
	{
		struct pie80211_drv_state scan = {.state = PIE80211_DRV_S_SCAN, .freq = 5745};
		struct pie80211_drv_tx management = {.flags = PIE80211_DRV_TX_MGMT};
		gate = true; driver->ops->set_state(pie,&scan); gate = false;
		pump(pie);
		assert(pie->basic_rate == 4);
		assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&management) == 0);
		pump(pie);
		assert(last_rate == 4 && packets_sent == 1);
		scan.freq = 2412;
		gate = true; driver->ops->set_state(pie,&scan); gate = false;
		pump(pie);
		assert(pie->basic_rate == 0);
		packets_sent = states = 0;
	}
	gate = true; driver->ops->set_state(pie,&state); gate = false;
	pump(pie);
	assert(states == 1);
	{
		uint8_t authentication[30] = {0xb0,0};
		struct pie80211_drv_tx management = {.flags = PIE80211_DRV_TX_MGMT};
		unsigned int old_errors = errors, old_packets = packets_sent;
		assert(driver->ops->tx(pie,pie80211_m_frombuf(authentication,sizeof(authentication),0),&management) == 0);
		pump(pie);
		assert(packets_sent == old_packets + 1 && errors == old_errors && pie->tx_count == 0 && pie->up);
		packets_sent = old_packets;
	}
	gate = true; request_key(driver,&key); gate = false;
	pump(pie);
	assert(keys == 1 && pie->hw.key_valid && memcmp(&pie->key,zero,sizeof(key)) == 0);
	{
		struct pie80211_drv_tx encrypted = {.cookie = 30,
		    .flags = PIE80211_DRV_TX_HW_KEY | PIE80211_DRV_TX_STATUS};
		unsigned int old_reports = reports;
		assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&encrypted) == 0);
		gate = true; request_key(driver,&key); gate = false;
		pump(pie);
		assert(keys == 2 && reports == old_reports + 1 && packets_sent == 0 && pie->tx_count == 0);
		gate = true; request_key(driver,&key); gate = false;
		key_hook = race_key;
		pump(pie);
		assert(keys == 2 && (pie->pending & P_KEY) != 0);
		pump(pie);
		assert(keys == 3 && pie->hardware_key_epoch == pie->key_epoch);
		encrypted.cookie = 31;
		assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&encrypted) == 0);
		gate = true; driver->ops->delete_key(pie,&key); gate = false;
		pump(pie);
		assert(!pie->hw.key_valid && reports == old_reports + 2 && packets_sent == 0 && pie->tx_count == 0);
		gate = true; request_key(driver,&key); gate = false;
		pump(pie);
		assert(keys == 4 && pie->hw.key_valid);
		gate = true; request_key(driver,&key); gate = false;
		defer_key_completion = true;
		pump(pie);
		assert(completed_key_generation == fake_key_generation && keys == 4);
		gate = true; request_key(driver,&key); gate = false;
		consume_key();
		assert(keys == 4);
		defer_key_completion = false;
		pump(pie);
		assert(keys == 5 && pie->hardware_key_epoch == pie->key_epoch);
		{
			struct rtw88_rx_packet packet = {.data = frame, .len = sizeof(frame), .freq = 2412, .decrypted = true};
			uint64_t old = fake_key_generation;
			unsigned int delivered = inputs;
			assert(receive(pie,&packet) == 0 && received_key_generation == fake_key_generation);
			quiesce_packet = &packet;
			gate = true; request_key(driver,&key); gate = false;
			pump(pie);
			quiesce_packet = NULL;
			assert(inputs == delivered + 2 && received_key_generation == old);
			assert(keys == 6 && pie->hardware_key_generation == fake_key_generation);
		}
	}
	assert(driver->ops->set_country(pie,"ZZ") == EINVAL);
	assert(driver->ops->set_country(pie,"XX") == EOPNOTSUPP);
	assert(memcmp(pie->country,"00",2) == 0 && pie->pending == 0);
	gate = true; driver->ops->set_state(pie,&state); gate = false;
	channel_hook = race_country;
	before = states;
	pump(pie);
	assert(states == before);
	pump(pie);
	assert(countries == 1 && memcmp(pie->country,"AU",2) == 0 && !pie->hw.key_valid);
	pump(pie);
	assert(starts == 2 && pie->up && !pie->paused);
	before = states;
	gate = true; driver->ops->set_state(pie,&state); gate = false;
	receive_hook = race_stop;
	pump(pie);
	assert(states == before);
	pump(pie);
	assert(!pie->up);
	gate = true; driver->ops->start(pie); gate = false;
	pump(pie);
	assert(pie->up && !pie->paused);
	before = reports;
	pie->paused = true;
	gate = true;
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == ENETDOWN);
	gate = false;
	assert(reports == before + 1);
	pie->paused = false;
	for (unsigned int i = 0; i < TX_QUEUE_SIZE; i++) {
		struct pie80211_drv_tx normal = {0};
		assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&normal) == 0);
	}
	gate = true;
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == ENOBUFS);
	gate = false;
	assert(reports == before + 2);
	purge_transmit(pie);
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	port_error = EAGAIN;
	for (unsigned int retry = 0; retry < TX_REPORT_SIZE + 1; retry++)
		pump(pie);
	assert(pie->report_next == 0 && packets_sent == 0 && pie->tx_count == 1);
	port_error = 0;
	pump(pie);
	assert(packets_sent == 1);
	now_ns += REPORT_TIMEOUT_NS;
	pump(pie);
	assert(reports == before + 3 && pie->report_next == 1);
	tx.cookie = 8;
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == 0);
	pump(pie);
	assert(pie->reports[1].cookie == 8 && pie->report_next == 2);
	{
		uint8_t delayed[] = {0,1};
		struct rtw88_rx_packet packet = {.data = delayed, .len = 2, .c2h = true};
		assert(receive(pie,&packet) == 0);
		assert(reports == before + 3 && pie->reports[1].cookie == 8);
		delayed[0] = 4;
		assert(receive(pie,&packet) == 0);
		assert(reports == before + 4 && pie->reports[1].cookie == 0);
	}
	test_report_slots(pie,driver);
	test_country_recovery(pie,driver);
	test_interrupt_mask(pie);
	test_tx_stall(pie);
	test_tx_watch_edges(pie);
	function.generation++;
	function.reset(function.reset_arg);
	before = restarts;
	pump(pie);
	assert(restarts == before + 1 && !pie->up && !pie->hw.key_valid);
	pump(pie);
	assert(pie->up && memcmp(pie->country,"AU",2) == 0);
	test_recovery_budget(pie,driver);
	pump(pie);
	assert(last_start_error == ENXIO && !family_running);
	before = reports;
	gate = true;
	assert(driver->ops->tx(pie,pie80211_m_frombuf(frame,sizeof(frame),0),&tx) == ENETDOWN);
	gate = false;
	assert(reports == before + 1);
	before = hardware_calls;
	pump(pie);
	assert(hardware_calls == before);
	assert(driver->ops->set_country(pie,"00") == ENETDOWN);
	fake_error = receive_error = 0;
	rtw88_pie_delete(pie);
	assert(allocation_count == 0 && !function.enabled && function.interrupt == NULL && function.reset == NULL);
	puts("rtw88: asynchronous commands, country races, key wiping, reports, reset bounds and detach tests passed");
	return 0;
}
