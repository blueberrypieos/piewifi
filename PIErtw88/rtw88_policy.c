/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <errno.h>
#include <limits.h>
#include "rtw88.h"

int
rtw88_board_validate(const struct rtw88_board_profile *profile)
{
	if (profile == NULL)
		return EOPNOTSUPP;
	if (profile->supported_rfe_mask == 0 ||
	    (profile->supported_rfe_mask & ~UINT32_C(0x55)) != 0 ||
	    (!profile->band_2g.supported && !profile->band_5g.supported))
		return EINVAL;
	return 0;
}

int
rtw88_board_limit(const struct rtw88_board_profile *profile, uint16_t freq,
    int16_t regulatory_eirp_mbm, int16_t *conducted_mbm)
{
	const struct rtw88_board_band *band;
	int32_t limit, gain;
	int error = rtw88_board_validate(profile);
	if (error != 0)
		return error;
	if (conducted_mbm == NULL)
		return EINVAL;
	if (freq >= 2412 && freq <= 2472 && (freq - 2407) % 5 == 0)
		band = &profile->band_2g;
	else if (freq >= 5180 && freq <= 5885 && (freq - 5000) % 5 == 0)
		band = &profile->band_5g;
	else
		return EOPNOTSUPP;
	if (!band->supported)
		return EOPNOTSUPP;
	gain = band->max_net_antenna_gain_mbi > 0 ? band->max_net_antenna_gain_mbi : 0;
	limit = (int32_t)regulatory_eirp_mbm - gain;
	if (limit > band->max_conducted_mbm)
		limit = band->max_conducted_mbm;
	if (limit < INT16_MIN || limit > INT16_MAX)
		return ERANGE;
	*conducted_mbm = (int16_t)limit;
	return 0;
}

int
rtw88_country_decode(const uint8_t raw[2], char country[2])
{
	if (raw == NULL || country == NULL)
		return EINVAL;
	if ((raw[0] == 0 && raw[1] == 0) ||
	    (raw[0] == 0xff && raw[1] == 0xff) ||
	    (raw[0] == '0' && raw[1] == '0')) {
		country[0] = country[1] = 0;
		return 0;
	}
	if (raw[0] < 'A' || raw[0] > 'Z' || raw[1] < 'A' || raw[1] > 'Z')
		return EILSEQ;
	country[0] = (char)raw[0];
	country[1] = (char)raw[1];
	return 0;
}

int
rtw88_basic_rate(uint16_t freq, const uint8_t *rates, size_t count,
    uint8_t *descriptor_rate)
{
	static const uint8_t legacy[] = {2,4,11,22,12,18,24,36,48,72,96,108};
	unsigned int pass, selected = 0;
	size_t at, rate;
	uint8_t lowest;
	if (rates == NULL || descriptor_rate == NULL || count == 0 || count > 32)
		return EOPNOTSUPP;
	for (pass = 0; pass < 2; pass++) {
		lowest = 0xff;
		for (at = 0; at < count; at++) {
			if (pass == 0 && (rates[at] & 0x80) == 0)
				continue;
			for (rate = freq > 5000 ? 4 : 0; rate < sizeof(legacy); rate++)
				if ((rates[at] & 0x7f) == legacy[rate] && legacy[rate] < lowest) {
					lowest = legacy[rate];
					selected = (unsigned int)rate;
				}
		}
		if (lowest != 0xff) {
			*descriptor_rate = (uint8_t)selected;
			return 0;
		}
	}
	return EOPNOTSUPP;
}
