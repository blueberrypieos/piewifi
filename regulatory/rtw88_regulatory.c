/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <sys/errno.h>
#include "rtw88_regulatory.h"
#include "rtw88_regulatory_data.h"

static const struct pie_rtw_reg_country *
find_country(const char code[2])
{
	size_t lo = 0;
	size_t hi = sizeof(pie_rtw_reg_countries) / sizeof(pie_rtw_reg_countries[0]);

	if (code == NULL)
		return NULL;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const struct pie_rtw_reg_country *c = &pie_rtw_reg_countries[mid];
		int cmp = (unsigned char)code[0] - (unsigned char)c->code[0];

		if (cmp == 0)
			cmp = (unsigned char)code[1] - (unsigned char)c->code[1];
		if (cmp == 0)
			return c;
		if (cmp < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

int
pie_rtw_reg_lookup(const char code[2], struct pie_rtw_reg_channel *channels,
    size_t capacity, size_t *count)
{
	const struct pie_rtw_reg_country *c;
	size_t i;

	if (count == NULL || (channels == NULL && capacity != 0))
		return EINVAL;
	c = find_country(code);
	if (c == NULL)
		return EINVAL;
	if (c->count > capacity)
		return ENOSPC;
	for (i = 0; i < c->count; i++)
		channels[i] = pie_rtw_reg_channels[c->first + i];
	*count = c->count;
	return 0;
}

int
pie_rtw_reg_limit(const char code[2], uint16_t freq, int16_t *max_eirp_mbm)
{
	const struct pie_rtw_reg_country *c;
	size_t lo, hi;

	if (max_eirp_mbm == NULL)
		return EINVAL;
	c = find_country(code);
	if (c == NULL)
		return EINVAL;
	lo = c->first;
	hi = lo + c->count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const struct pie_rtw_reg_channel *ch = &pie_rtw_reg_channels[mid];

		if (freq == ch->freq) {
			*max_eirp_mbm = ch->max_eirp_mbm;
			return 0;
		}
		if (freq < ch->freq)
			hi = mid;
		else
			lo = mid + 1;
	}
	return ENOENT;
}
