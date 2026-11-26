/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef PIE_RTW88_H
#define PIE_RTW88_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTW88_BIT(n) (UINT32_C(1) << (n))
#define RTW88_TXDESC 48
#define RTW88_RXDESC 24
#define RTW88_MAX_FRAME 4096
#define RTW88_RX_BUFFER 16384
#define RTW88_EFUSE_SIZE 512
#define RTW88_LOCAL UINT32_C(0x10250000)
#define RTW88_MAC UINT32_C(0x10260000)
#define RTW88_SDIO_VENDOR 0x024c
#define RTW88_SDIO_DEVICE 0xc821
#define RTW88_SDIO_FUNCTION 1
#define RTW88_SDIO_HIMR (RTW88_LOCAL + 0x14)
#define RTW88_SDIO_HISR (RTW88_LOCAL + 0x18)
#define RTW88_IRQ_MASK RTW88_BIT(0)

struct rtw88_io {
	int (*read_byte)(void *ctx, uint32_t addr, uint8_t *value);
	int (*write_byte)(void *ctx, uint32_t addr, uint8_t value);
	int (*read)(void *ctx, uint32_t addr, void *buf, size_t len, bool incr);
	int (*write)(void *ctx, uint32_t addr, const void *buf, size_t len, bool incr);
	void (*delay)(void *ctx, unsigned int usec);
	void *ctx;
	size_t max_request;
	unsigned int block_size;
};

struct rtw88_board_band {
	bool supported;
	int16_t max_net_antenna_gain_mbi;
	int16_t max_conducted_mbm;
};

struct rtw88_board_profile {
	uint32_t supported_rfe_mask;
	struct rtw88_board_band band_2g;
	struct rtw88_board_band band_5g;
};

struct rtw88_hw {
	struct rtw88_io io;
	const struct rtw88_board_profile *board;
	int error;
	uint32_t failed_register;
	bool powered;
	bool uhs;
	bool key_valid;
	bool firmware_ready;
	bool rfe_btg;
	bool rx_stopped;
	bool rx_was_enabled;
	bool tx_paused;
	uint16_t tx_used[6];
	uint8_t cut;
	uint8_t rfe;
	uint8_t package;
	uint8_t rx_addr;
	uint8_t slot_time;
	uint8_t rts_rate;
	bool protection;
	uint8_t mac[6];
	uint8_t efuse[RTW88_EFUSE_SIZE];
	uint8_t calibration[RTW88_EFUSE_SIZE];
	uint8_t country[2];
	uint16_t freq;
	uint16_t h2c_seq;
	uint32_t ch_param[3];
	uint8_t scratch[RTW88_RX_BUFFER];
};

struct rtw88_fw_segment {
	const uint8_t *data;
	uint32_t destination;
	size_t size;
};

struct rtw88_firmware {
	uint16_t version;
	uint8_t subversion;
	size_t count;
	struct rtw88_fw_segment segment[3];
};

struct rtw88_rx_packet {
	const uint8_t *data;
	size_t len;
	uint16_t freq;
	int8_t rssi;
	uint32_t timestamp;
	bool c2h;
	bool decrypted;
};

struct rtw88_tx_packet {
	uint8_t qsel;
	uint8_t rate;
	uint8_t tid;
	uint8_t report_id;
	uint8_t rts_rate;
	bool no_ack;
	bool protection;
	bool hardware_key;
	bool report;
	bool raw;
	bool h2c;
};

struct rtw_table;

uint16_t rtw88_get16(const void *buf);
uint32_t rtw88_get32(const void *buf);
void rtw88_put16(void *buf, uint16_t value);
void rtw88_put32(void *buf, uint32_t value);
uint32_t rtw88_field(uint32_t value, uint32_t mask);
void rtw88_fail(struct rtw88_hw *hw, int error);
uint8_t rtw88_read8(struct rtw88_hw *hw, uint32_t addr);
uint16_t rtw88_read16(struct rtw88_hw *hw, uint32_t addr);
uint32_t rtw88_read32(struct rtw88_hw *hw, uint32_t addr);
void rtw88_write8(struct rtw88_hw *hw, uint32_t addr, uint8_t value);
void rtw88_write16(struct rtw88_hw *hw, uint32_t addr, uint16_t value);
void rtw88_write32(struct rtw88_hw *hw, uint32_t addr, uint32_t value);
int rtw88_irq_mask(struct rtw88_hw *hw, uint32_t mask);
void rtw88_mask8(struct rtw88_hw *hw, uint32_t addr, uint8_t mask, uint8_t value);
void rtw88_mask16(struct rtw88_hw *hw, uint32_t addr, uint16_t mask, uint16_t value);
void rtw88_mask32(struct rtw88_hw *hw, uint32_t addr, uint32_t mask, uint32_t value);
int rtw88_poll(struct rtw88_hw *hw, uint32_t addr, uint32_t mask, uint32_t value,
    unsigned int width, unsigned int interval_us, unsigned int tries);
void rtw88_delay(struct rtw88_hw *hw, unsigned int usec);
size_t rtw88_align(const struct rtw88_hw *hw, size_t len);
int rtw88_read_port(struct rtw88_hw *hw, void *buf, size_t len);
int rtw88_write_port(struct rtw88_hw *hw, uint8_t qsel, const void *buf,
    size_t len, bool check_space);
int rtw88_tx_free(struct rtw88_hw *hw, uint32_t pages[3]);
int rtw88_decode_efuse(const uint8_t *physical, size_t physical_len,
    uint8_t *logical, size_t logical_len);
int rtw88_read_efuse(struct rtw88_hw *hw);
int rtw88_parse_firmware(const uint8_t *buf, size_t len,
    struct rtw88_firmware *fw);
int rtw88_lte_read(struct rtw88_hw *hw, uint16_t addr, uint32_t *value);
int rtw88_lte_write(struct rtw88_hw *hw, uint16_t addr, uint32_t value);
int rtw88_wifi_only(struct rtw88_hw *hw);
int rtw88_download_firmware(struct rtw88_hw *hw, const uint8_t *buf, size_t len);
int rtw88_power(struct rtw88_hw *hw, bool on);
int rtw88_mac_init(struct rtw88_hw *hw);
int rtw88_phy_init(struct rtw88_hw *hw);
int rtw88_phy_channel(struct rtw88_hw *hw, uint16_t freq, int16_t max_eirp_mbm);
int rtw88_calibrate(struct rtw88_hw *hw);
uint32_t rtw88_rf_read(struct rtw88_hw *hw, uint8_t addr);
void rtw88_rf_write(struct rtw88_hw *hw, uint8_t addr, uint32_t mask, uint32_t value);
int rtw88_table(struct rtw88_hw *hw, const struct rtw_table *table);
int rtw88_key(struct rtw88_hw *hw, const uint8_t addr[6], const uint8_t key[16]);
int rtw88_rx_quiesce(struct rtw88_hw *hw,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg);
int rtw88_rx_resume(struct rtw88_hw *hw);
int rtw88_tx_quiesce(struct rtw88_hw *hw);
int rtw88_key_clear(struct rtw88_hw *hw);
int rtw88_h2c(struct rtw88_hw *hw, uint8_t id, const uint8_t *data, size_t len);
int rtw88_state(struct rtw88_hw *hw, int state, const uint8_t bssid[6],
    uint16_t aid, uint16_t beacon_interval, uint8_t erp, uint16_t response_rates);
int rtw88_edca(struct rtw88_hw *hw, unsigned int ac, uint8_t aifsn,
    uint8_t ecwmin, uint8_t ecwmax, uint16_t txop);
int rtw88_board_validate(const struct rtw88_board_profile *profile);
int rtw88_board_limit(const struct rtw88_board_profile *profile, uint16_t freq, int16_t regulatory_eirp_mbm, int16_t *conducted_mbm);
int rtw88_country_decode(const uint8_t raw[2], char country[2]);
int rtw88_basic_rate(uint16_t freq, const uint8_t *rates, size_t count, uint8_t *descriptor_rate);
int rtw88_tx_descriptor(uint8_t *dst, size_t capacity, const uint8_t *frame,
    size_t len, const struct rtw88_tx_packet *tx);
int rtw88_rx_parse(const uint8_t *buf, size_t len, uint16_t freq, uint8_t rfe,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg);
int rtw88_rx_drain(struct rtw88_hw *hw,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg);
int rtw88_tx_report(const uint8_t *data, size_t len, uint8_t *sequence, bool *acked);

#endif
