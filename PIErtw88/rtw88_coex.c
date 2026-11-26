/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018-2019 Realtek Corporation
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 * Modified 2026-11-24: Native PIE LTE register access and WiFi-only antenna control.
 */
#include <errno.h>
#include "rtw88.h"
#include "rtw88_registers.h"

int
rtw88_lte_read(struct rtw88_hw *hw, uint16_t addr, uint32_t *value)
{
	int error = rtw88_poll(hw, LTECOEX_ACCESS_CTRL, LTECOEX_READY,
	    LTECOEX_READY, 4, 10, 1000);
	if (error != 0)
		return error;
	rtw88_write32(hw, LTECOEX_ACCESS_CTRL, 0x800f0000U | addr);
	*value = rtw88_read32(hw, LTECOEX_READ_DATA);
	return hw->error;
}

int
rtw88_lte_write(struct rtw88_hw *hw, uint16_t addr, uint32_t value)
{
	int error = rtw88_poll(hw, LTECOEX_ACCESS_CTRL, LTECOEX_READY,
	    LTECOEX_READY, 4, 10, 1000);
	if (error != 0)
		return error;
	rtw88_write32(hw, LTECOEX_WRITE_DATA, value);
	rtw88_write32(hw, LTECOEX_ACCESS_CTRL, 0xc00f0000U | addr);
	return hw->error;
}

int
rtw88_wifi_only(struct rtw88_hw *hw)
{
	uint32_t grant;
	int error;
	rtw88_mask8(hw, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION, 1);
	rtw88_mask8(hw, REG_BT_TDMA_TIME, BIT_MASK_SAMPLE_RATE, 5);
	rtw88_write8(hw, REG_BT_STAT_CTRL, BT_CNT_ENABLE);
	rtw88_write32(hw, REG_GPIO_MUXCFG, rtw88_read32(hw, REG_GPIO_MUXCFG) |
	    BIT_BT_PTA_EN | BIT_PO_BT_PTA_PINS);
	rtw88_mask8(hw, REG_QUEUE_CTRL, BIT_PTA_WL_TX_EN, 1);
	rtw88_mask8(hw, REG_QUEUE_CTRL, BIT_PTA_EDCCA_EN, 0);
	rtw88_mask16(hw, REG_BT_COEX_V2, BIT_GNT_BT_POLARITY, 1);
	rtw88_mask8(hw, REG_BT_COEX_TABLE_H + 3, BIT_BCN_QUEUE, BCN_PRI_EN);
	error = rtw88_lte_read(hw, LTE_COEX_CTRL, &grant);
	if (error != 0)
		return error;
	grant = (grant & ~0xff00U) | 0x7700U;
	error = rtw88_lte_write(hw, LTE_COEX_CTRL, grant);
	if (error != 0)
		return error;
	rtw88_mask8(hw, REG_SYS_SDIO_CTRL + 3, BIT_LTE_MUX_CTRL_PATH >> 24, 1);
	if (hw->rfe != 6) {
		rtw88_mask32(hw, REG_LED_CFG, BIT_DPDT_SEL_EN, 0);
		rtw88_mask32(hw, REG_LED_CFG, BIT_DPDT_WL_SEL, 1);
		rtw88_mask8(hw, REG_RFE_CTRL8, BIT_MASK_RFE_SEL89, DPDT_CTRL_PIN);
		rtw88_mask32(hw, REG_RFE_CTRL8, BIT_MASK_R_RFE_SEL_15, 2);
		rtw88_mask8(hw, REG_CTRL_TYPE, BIT_CTRL_TYPE1 | BIT_CTRL_TYPE2, 3);
	}
	return hw->error;
}
