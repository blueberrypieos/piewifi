/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rtw88_regulatory.h"

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

int
main(void)
{
	struct pie_rtw_reg_channel channels[PIE_RTW_REG_MAX_CHANNELS];
	struct pie_rtw_reg_channel original[PIE_RTW_REG_MAX_CHANNELS];
	size_t count = 12345;
	int16_t power = 12345;

	memset(channels, 0xa5, sizeof(channels));
	memcpy(original, channels, sizeof(channels));
	CHECK(pie_rtw_reg_lookup("ZZ", channels, PIE_RTW_REG_MAX_CHANNELS, &count) == EINVAL);
	CHECK(count == 12345 && memcmp(channels, original, sizeof(channels)) == 0);
	CHECK(pie_rtw_reg_lookup(NULL, channels, PIE_RTW_REG_MAX_CHANNELS, &count) == EINVAL);
	CHECK(pie_rtw_reg_lookup("au", channels, PIE_RTW_REG_MAX_CHANNELS, &count) == EINVAL);
	CHECK(pie_rtw_reg_lookup("00", NULL, 1, &count) == EINVAL);
	CHECK(pie_rtw_reg_lookup("00", channels, PIE_RTW_REG_MAX_CHANNELS, NULL) == EINVAL);
	CHECK(pie_rtw_reg_limit("ZZ", 2412, &power) == EINVAL && power == 12345);
	CHECK(pie_rtw_reg_limit(NULL, 2412, &power) == EINVAL && power == 12345);
	CHECK(pie_rtw_reg_limit("00", 2412, NULL) == EINVAL);
#ifdef EMPTY_COUNTRY_TEST
	CHECK(pie_rtw_reg_lookup("00", NULL, 0, &count) == 0 && count == 0);
	CHECK(pie_rtw_reg_lookup("00", channels, PIE_RTW_REG_MAX_CHANNELS, &count) == 0);
	CHECK(count == 0 && memcmp(channels, original, sizeof(channels)) == 0);
	CHECK(pie_rtw_reg_limit("00", 2412, &power) == ENOENT && power == 12345);
#else
	CHECK(pie_rtw_reg_lookup("00", channels, 0, &count) == ENOSPC);
	CHECK(count == 12345 && memcmp(channels, original, sizeof(channels)) == 0);
	CHECK(pie_rtw_reg_lookup("00", NULL, 0, &count) == ENOSPC && count == 12345);
	CHECK(pie_rtw_reg_lookup("00", channels, PIE_RTW_REG_MAX_CHANNELS, &count) == 0);
	CHECK(count == 11);
	for (size_t i = 0; i < count; i++) {
		CHECK(channels[i].freq == 2412 + 5 * i);
		CHECK(channels[i].max_eirp_mbm == 2000);
	}
	CHECK(pie_rtw_reg_limit("00", 2462, &power) == 0 && power == 2000);
	power = 12345;
	CHECK(pie_rtw_reg_limit("00", 2467, &power) == ENOENT && power == 12345);
	CHECK(pie_rtw_reg_lookup("AU", channels, PIE_RTW_REG_MAX_CHANNELS, &count) == 0);
	CHECK(count > 13 && count <= PIE_RTW_REG_MAX_CHANNELS);
	for (size_t i = 1; i < count; i++)
		CHECK(channels[i - 1].freq < channels[i].freq);
	CHECK(pie_rtw_reg_limit("AU", 5745, &power) == 0 && power == 3602);
	power = 12345;
	CHECK(pie_rtw_reg_limit("AU", 5180, &power) == ENOENT && power == 12345);
	CHECK(pie_rtw_reg_limit("AU", 5260, &power) == ENOENT && power == 12345);
	CHECK(pie_rtw_reg_limit("AU", 5955, &power) == ENOENT && power == 12345);
#endif
	puts("regulatory lookup: passed");
	return 0;
}
