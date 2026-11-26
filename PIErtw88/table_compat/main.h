/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018-2019 Realtek Corporation
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 * Modified 2026-11-24: Native PIE table types and declaration adapters.
 */
#ifndef PIE_RTW88_TABLE_COMPAT_H
#define PIE_RTW88_TABLE_COMPAT_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef int8_t s8;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct rtw_table {
	const void *data;
	size_t size;
	unsigned int kind;
};
struct rtw_phy_pg_cfg_pair {
	u32 band, rf_path, tx_num, addr, bitmask, data;
};
struct rtw_txpwr_lmt_cfg_pair {
	u8 regd, band, bw, rs, ch;
	s8 txpwr_lmt;
};
#define rtw_phy_cfg_mac 0
#define rtw_phy_cfg_agc 1
#define rtw_phy_cfg_bb 2
#define RTW_DECL_TABLE_PHY_COND(name, cfg) \
	const struct rtw_table name ## _tbl = { name, ARRAY_SIZE(name), cfg }
#define RTW_DECL_TABLE_RF_RADIO(name, path) \
	const struct rtw_table name ## _tbl = { name, ARRAY_SIZE(name), 3 }
#define RTW_DECL_TABLE_BB_PG(name) \
	const struct rtw_table name ## _tbl = { name, ARRAY_SIZE(name), 4 }
#define RTW_DECL_TABLE_TXPWR_LMT(name) \
	const struct rtw_table name ## _tbl = { name, ARRAY_SIZE(name), 5 }
#endif
