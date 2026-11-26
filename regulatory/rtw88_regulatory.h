/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * pie_rtw_reg_lookup() copies the permitted 20 MHz channels for an exact
 * two-byte uppercase country code or "00". It returns zero or an errno.
 * ENOSPC leaves every output untouched. A known country with no channels
 * succeeds with count zero. pie_rtw_reg_limit() returns ENOENT for a
 * frequency absent from that country's table. All errors leave outputs
 * untouched. EIRP is a ceiling in hundredths of a dBm, before the driver
 * applies chipset, efuse, antenna and board restrictions.
 */
#ifndef PIE_RTW88_REGULATORY_H
#define PIE_RTW88_REGULATORY_H

#include <stddef.h>
#include <stdint.h>

#define PIE_RTW_REG_MAX_CHANNELS 64

struct pie_rtw_reg_channel {
	uint16_t freq;
	int16_t max_eirp_mbm;
};

#ifdef __cplusplus
extern "C" {
#endif

int pie_rtw_reg_lookup(const char country[2], struct pie_rtw_reg_channel *channels,
    size_t capacity, size_t *count);
int pie_rtw_reg_limit(const char country[2], uint16_t freq, int16_t *max_eirp_mbm);

#ifdef __cplusplus
}
#endif

#endif
