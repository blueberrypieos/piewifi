/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018-2021 Realtek Corporation
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 * Modified 2026-11-24: Native PIE PHY tables, serial RF writes and calibrated channel control.
 */
#include <errno.h>
#include <string.h>
#include "rtw88.h"
#include "rtw88_registers.h"
#include "table_compat/main.h"
#include "rtw8821c_table.h"

typedef uint16_t u16;
#define RTW_CHANNEL_WIDTH_20 0
#define RTW_CHANNEL_WIDTH_40 1
#define RTW_CHANNEL_WIDTH_80 2
#define RTW_CHANNEL_WIDTH_5 3
#define RTW_CHANNEL_WIDTH_10 4
#define SWITCH_TO_BTG 0
#define SWITCH_TO_WLG 1
#define SWITCH_TO_WLA 2
#define SWITCH_TO_BT 3
#define rtw_read8 rtw88_read8
#define rtw_read32 rtw88_read32
#define rtw_read32_mask(h,a,m) ((rtw88_read32(h,a) & (m)) >> __builtin_ctz((uint32_t)(m)))
#define rtw_write8 rtw88_write8
#define rtw_write32 rtw88_write32
#define rtw_write32_mask rtw88_mask32
#define rtw_write32_set(h,a,m) rtw88_write32(h,a,rtw88_read32(h,a) | (m))
#define rtw_write32_clr(h,a,m) rtw88_write32(h,a,rtw88_read32(h,a) & ~(m))
#define rtw_read_rf(h,p,a,m) ((rtw88_rf_read(h,a) & (m)) >> __builtin_ctz((uint32_t)(m)))
#define rtw_write_rf(h,p,a,m,v) rtw88_rf_write(h,a,m,v)
#define rtw_regd_srrc(h) ((h)->country[0] == 'C' && (h)->country[1] == 'N')
#include "rtw88_channel.inc"

uint32_t
rtw88_rf_read(struct rtw88_hw *hw, uint8_t addr)
{
	return rtw88_read32(hw, 0x2800 + ((uint32_t)addr << 2)) & 0xfffff;
}

void
rtw88_rf_write(struct rtw88_hw *hw, uint8_t addr, uint32_t mask, uint32_t value)
{
	uint32_t old;
	mask &= 0xfffff;
	if (mask == 0) {
		rtw88_fail(hw, EINVAL);
		return;
	}
	old = mask == 0xfffff ? 0 : rtw88_rf_read(hw, addr);
	if (mask != 0xfffff && old == 0xfffff) {
		rtw88_fail(hw, EIO);
		return;
	}
	old = (old & ~mask) | rtw88_field(value, mask);
	rtw88_write32(hw, 0xc90, (uint32_t)addr << 20 | (old & 0xfffff));
	rtw88_delay(hw, 13);
}

static bool
condition(struct rtw88_hw *hw, uint32_t value)
{
	uint32_t cut = (value >> 24) & 15;
	uint32_t package = (value >> 12) & 15;
	uint32_t interface = (value >> 8) & 15;
	return (cut == 0 || cut == (hw->cut != 0 ? hw->cut : 15)) &&
	    (package == 0 || package == (hw->package != 0 ? hw->package : 15)) &&
	    (interface == 0 || interface == 4) && (value & 255) == hw->rfe;
}

int
rtw88_table(struct rtw88_hw *hw, const struct rtw_table *table)
{
	const uint32_t *data;
	uint32_t positive = 0, addr, value;
	size_t at;
	bool matched = true, skipped = false;
	if (table == NULL || table->kind > 3 || (table->size & 1) != 0)
		return EINVAL;
	data = table->data;
	for (at = 0; at < table->size && hw->error == 0; at += 2) {
		addr = data[at];
		value = data[at + 1];
		if ((addr & BIT(31)) != 0) {
			switch ((addr >> 28) & 3) {
			case 3:
				matched = true;
				skipped = false;
				break;
			case 2:
				matched = !skipped;
				break;
			default:
				positive = addr;
				break;
			}
			continue;
		}
		if ((addr & BIT(30)) != 0) {
			matched = !skipped && condition(hw, positive);
			if (matched)
				skipped = true;
			continue;
		}
		if (!matched)
			continue;
		if (table->kind == 0) {
			rtw88_write8(hw, addr, (uint8_t)value);
		} else if (table->kind == 1) {
			rtw88_write32(hw, addr, value);
		} else if (table->kind == 2) {
			if (addr >= 0xf9 && addr <= 0xfe) {
				static const unsigned int delay[] = {1, 5, 50, 1000, 5000, 50000};
				rtw88_delay(hw, delay[addr - 0xf9]);
			} else {
				rtw88_write32(hw, addr, value);
			}
		} else if (addr == 0xffe) {
			rtw88_delay(hw, 50000);
		} else if (addr == 0xfe) {
			rtw88_delay(hw, 100);
		} else {
			rtw88_rf_write(hw, (uint8_t)addr, 0xfffff, value);
			rtw88_delay(hw, 1);
		}
	}
	return hw->error;
}

int
rtw88_phy_init(struct rtw88_hw *hw)
{
	uint8_t val, crystal;
	int error;
	val = rtw88_read8(hw, REG_SYS_FUNC_EN) | BIT_FEN_PCIEA;
	rtw88_write8(hw, REG_SYS_FUNC_EN, val);
	val |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
	rtw88_write8(hw, REG_SYS_FUNC_EN, val);
	rtw88_write8(hw, REG_SYS_FUNC_EN, (uint8_t)(val & ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST)));
	rtw88_write8(hw, REG_SYS_FUNC_EN, val);
	rtw88_write8(hw, REG_RF_CTRL, BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	rtw88_delay(hw, 10);
	rtw88_write8(hw, REG_WLRF1 + 3, BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	rtw88_delay(hw, 10);
	rtw_write32_clr(hw, REG_RXPSEL, BIT_RX_PSEL_RST);
	error = rtw88_table(hw, &rtw8821c_mac_tbl);
	if (error == 0)
		error = rtw88_table(hw, &rtw8821c_bb_tbl);
	if (error == 0)
		error = rtw88_table(hw, &rtw8821c_agc_tbl);
	if (error == 0 && (hw->rfe == 2 || hw->rfe == 4))
		error = rtw88_table(hw, &rtw8821c_agc_btg_type2_tbl);
	if (error == 0)
		error = rtw88_table(hw, &rtw8821c_rf_a_tbl);
	if (error != 0)
		return error;
	crystal = hw->efuse[0xb9] == 0xff ? 0 : hw->efuse[0xb9] & 0x3f;
	rtw88_mask32(hw, REG_AFE_XTAL_CTRL, 0x7e000000, crystal);
	rtw88_mask32(hw, REG_AFE_PLL_CTRL, 0x7e, crystal);
	rtw88_mask32(hw, REG_CCK0_FAREPORT, BIT(18) | BIT(22), 0);
	rtw_write32_set(hw, REG_RXPSEL, BIT_RX_PSEL_RST);
	hw->ch_param[0] = rtw88_read32(hw, REG_TXSF2);
	hw->ch_param[1] = rtw88_read32(hw, REG_TXSF6);
	hw->ch_param[2] = rtw88_read32(hw, REG_TXFILTER);
	rtw88_write32(hw, 0x1c94, 0xafffafff);
	return rtw88_wifi_only(hw);
}

static int
channel_group(unsigned int channel)
{
	if (channel <= 14)
		return channel <= 2 ? 0 : channel <= 5 ? 1 : channel <= 8 ? 2 : channel <= 11 ? 3 : 4;
	if (channel >= 36 && channel <= 64)
		return (int)((channel - 36) / 8);
	if (channel >= 100 && channel <= 144)
		return (int)(4 + (channel - 100) / 8);
	if (channel >= 149 && channel <= 177)
		return (int)(10 + (channel - 149) / 8);
	return -1;
}

static int
bcd(uint8_t value)
{
	if ((value & 15) > 9 || (value >> 4) > 9)
		return -1;
	return (value >> 4) * 10 + (value & 15);
}

static int
signed_nibble(uint8_t value)
{
	return (value & 8) != 0 ? (int)(value & 15) - 16 : value & 15;
}

static int
calibrated_power(struct rtw88_hw *hw, unsigned int channel, int16_t ceiling)
{
	const struct rtw_phy_pg_cfg_pair *pg = rtw8821c_bb_pg_type0_tbl.data;
	const struct rtw_txpwr_lmt_cfg_pair *limits = rtw8821c_txpwr_lmt_type0_tbl.data;
	int rates[12], limit[2] = {127, 127}, index[12];
	unsigned int band = channel <= 14 ? 0 : 1;
	unsigned int rate, section, word;
	int group = channel_group(channel), base, reference, offset, limit_offset, sar_offset;
	size_t at;
	if (group < 0 || ceiling < 0)
		return EOPNOTSUPP;
	for (at = 0; at < 12; at++)
		rates[at] = -1;
	for (at = 0; at < rtw8821c_bb_pg_type0_tbl.size; at++) {
		if (pg[at].band != band || pg[at].rf_path != 0 || pg[at].tx_num != 0)
			continue;
		if (pg[at].addr < 0xc20 || pg[at].addr > 0xc28 || (pg[at].addr & 3) != 0)
			continue;
		for (rate = 0; rate < 4; rate++)
			rates[pg[at].addr - 0xc20 + rate] = bcd((uint8_t)(pg[at].data >> (8 * rate)));
	}
	for (at = 0; at < rtw8821c_txpwr_lmt_type0_tbl.size; at++) {
		if (limits[at].band == band && limits[at].bw == 0 && limits[at].ch == channel && limits[at].rs < 2 && limits[at].txpwr_lmt < limit[limits[at].rs])
			limit[limits[at].rs] = limits[at].txpwr_lmt;
	}
	for (rate = band == 0 ? 0 : 4; rate < 12; rate++) {
		section = rate < 4 ? 0 : 1;
		reference = rates[section == 0 ? 3 : 11];
		if (rates[rate] < 0 || reference < 0 || limit[section] == 127)
			return EOPNOTSUPP;
		if (band == 0) {
			base = hw->calibration[0x10 + (section == 0 ? group : 6 + group)];
			if (section == 1)
				base += signed_nibble(hw->calibration[0x10 + 11]);
		} else {
			base = hw->calibration[0x10 + 18 + group];
			base += signed_nibble(hw->calibration[0x10 + 18 + 14]);
		}
		if (base < 0 || base > 63)
			return EOPNOTSUPP;
		offset = rates[rate] - reference;
		limit_offset = limit[section] - reference;
		sar_offset = ceiling / 50 - reference;
		if (offset > limit_offset)
			offset = limit_offset;
		if (offset > sar_offset)
			offset = sar_offset;
		index[rate] = base + offset;
		if (index[rate] < 0)
			return EOPNOTSUPP;
		if (index[rate] > 63)
			index[rate] = 63;
	}
	for (rate = band == 0 ? 0 : 4; rate < 12; rate += 4) {
		word = (unsigned int)index[rate] | (unsigned int)index[rate + 1] << 8 |
		    (unsigned int)index[rate + 2] << 16 | (unsigned int)index[rate + 3] << 24;
		rtw88_write32(hw, 0x1d00 + rate, word);
	}
	return hw->error;
}

int
rtw88_phy_channel(struct rtw88_hw *hw, uint16_t freq, int16_t max_eirp_mbm)
{
	unsigned int channel;
	uint8_t swing;
	static const uint32_t swing_table[] = {0x200, 0x16a, 0x101, 0x0b6};
	int16_t conducted;
	int error = rtw88_board_limit(hw->board, freq, max_eirp_mbm, &conducted);
	if (error != 0)
		return error;
	if (freq >= 2412 && freq <= 2472 && (freq - 2407) % 5 == 0)
		channel = (freq - 2407) / 5;
	else if (freq >= 5180 && freq <= 5885 && (freq - 5000) % 5 == 0)
		channel = (freq - 5000) / 5;
	else
		return EOPNOTSUPP;
	error = calibrated_power(hw, channel, conducted);
	if (error != 0)
		return error;
	rtw8821c_set_channel_bb(hw, (uint8_t)channel, RTW_CHANNEL_WIDTH_20, 0);
	swing = hw->efuse[channel <= 14 ? 0xc5 : 0xc6];
	if (swing == 0xff || swing > 9)
		swing = 0;
	rtw88_mask32(hw, REG_TXSCALE_A, GENMASK(31, 21), swing_table[swing / 3]);
	rtw88_write8(hw, REG_DATA_SC, 0);
	rtw_write32_clr(hw, REG_WMAC_TRXPTCL_CTL, BIT_RFMOD);
	rtw88_mask32(hw, REG_AFE_CTRL1, BIT_MAC_CLK_SEL, MAC_CLK_HW_DEF_80M);
	rtw88_write8(hw, REG_USTIME_TSF, MAC_CLK_SPEED);
	rtw88_write8(hw, REG_USTIME_EDCA, MAC_CLK_SPEED);
	rtw88_mask8(hw, REG_CCK_CHECK, BIT_CHECK_CCK_EN, channel > 14 ? 1 : 0);
	rtw8821c_set_channel_rf(hw, (uint8_t)channel, RTW_CHANNEL_WIDTH_20);
	rtw8821c_set_channel_rxdfir(hw, RTW_CHANNEL_WIDTH_20);
	if (hw->error == 0)
		hw->freq = freq;
	return hw->error;
}

int
rtw88_calibrate(struct rtw88_hw *hw)
{
	uint8_t data = 0;
	unsigned int count;
	int error = rtw88_h2c(hw, H2C_PKT_IQK, &data, sizeof(data));
	if (error != 0)
		return error;
	for (count = 0; count < 300 && hw->error == 0; count++) {
		if (rtw88_rf_read(hw, RF_DTXLOK) == 0xabcde)
			break;
		rtw88_delay(hw, 20000);
	}
	rtw88_rf_write(hw, RF_DTXLOK, 0xfffff, 0);
	return hw->error;
}
