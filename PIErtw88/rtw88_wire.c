/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018-2019 Realtek Corporation
 * Copyright (c) 2021 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 * Copyright (c) 2021 Jernej Skrabec <jernej.skrabec@gmail.com>
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 * Modified 2026-11-24: Native PIE firmware, efuse and packet format validation with receive aggregate parsing.
 */
#include <errno.h>
#include <string.h>
#include "rtw88.h"
#include "rtw88_registers.h"

int
rtw88_decode_efuse(const uint8_t *physical, size_t physical_len,
    uint8_t *logical, size_t logical_len)
{
	size_t at = 0, word, offset, block;
	uint8_t a, b, enabled;
	if (physical == NULL || logical == NULL || physical_len == 0)
		return EINVAL;
	memset(logical, 0xff, logical_len);
	while (at < physical_len) {
		a = physical[at++];
		if (a == 0xff)
			return 0;
		if ((a & 0x1f) == 0x0f) {
			if (at == physical_len)
				return EINVAL;
			b = physical[at++];
			if (b == 0xff)
				return 0;
			block = ((b & 0xf0) >> 1) | ((a >> 5) & 7);
			enabled = b & 15;
		} else {
			block = a >> 4;
			enabled = a & 15;
		}
		for (word = 0; word < 4; word++) {
			if ((enabled & (1U << word)) != 0)
				continue;
			offset = block * 8 + word * 2;
			if (physical_len - at < 2 || offset >= logical_len || logical_len - offset < 2)
				return EINVAL;
			logical[offset] = physical[at++];
			logical[offset + 1] = physical[at++];
		}
	}
	return 0;
}

int
rtw88_parse_firmware(const uint8_t *buf, size_t len, struct rtw88_firmware *fw)
{
	struct rtw88_firmware parsed;
	uint32_t sizes[3], addresses[3];
	size_t offset = FW_HDR_SIZE, i;
	if (buf == NULL || fw == NULL || len < FW_HDR_SIZE)
		return EINVAL;
	memset(&parsed, 0, sizeof(parsed));
	sizes[0] = rtw88_get32(buf + 0x24);
	sizes[1] = rtw88_get32(buf + 0x30);
	sizes[2] = (buf[0x18] & BIT(4)) != 0 ? rtw88_get32(buf + 0x34) : 0;
	addresses[0] = rtw88_get32(buf + 0x20) & ~BIT(31);
	addresses[1] = rtw88_get32(buf + 0x3c) & ~BIT(31);
	addresses[2] = rtw88_get32(buf + 0x38) & ~BIT(31);
	for (i = 0; i < 3; i++) {
		size_t size;
		if (i == 2 && sizes[i] == 0)
			continue;
		size = (size_t)sizes[i] + FW_HDR_CHKSUM_SIZE;
		if (size < sizes[i] || size > len - offset || addresses[i] > UINT32_MAX - size)
			return EINVAL;
		parsed.segment[parsed.count].data = buf + offset;
		parsed.segment[parsed.count].destination = addresses[i];
		parsed.segment[parsed.count].size = size;
		parsed.count++;
		offset += size;
	}
	if (offset != len || parsed.count < 2)
		return EINVAL;
	parsed.version = rtw88_get16(buf + 4);
	parsed.subversion = buf[6];
	*fw = parsed;
	return 0;
}

int
rtw88_tx_descriptor(uint8_t *dst, size_t capacity, const uint8_t *frame,
    size_t len, const struct rtw88_tx_packet *tx)
{
	uint16_t check = 0;
	uint32_t word;
	size_t i;
	if (dst == NULL || tx == NULL || len == 0 || len > UINT16_MAX ||
	    capacity < RTW88_TXDESC || (!tx->raw && (frame == NULL || len < 24)))
		return EINVAL;
	memset(dst, 0, RTW88_TXDESC);
	word = (uint32_t)len;
	if (!tx->h2c)
		word |= (RTW88_TXDESC << 16) | BIT(26);
	if (!tx->raw && (frame[4] & 1) != 0)
		word |= BIT(24);
	rtw88_put32(dst, word);
	word = (uint32_t)tx->qsel << 8;
	if (tx->hardware_key)
		word |= 3 << 22;
	rtw88_put32(dst + 4, word);
	rtw88_put32(dst + 8, tx->report ? BIT(19) : 0);
	rtw88_put32(dst + 12, tx->h2c ? 0 : BIT(8) | BIT(10) | (tx->protection ? BIT(12) : 0));
	rtw88_put32(dst + 16, tx->rate | (tx->protection ? (uint32_t)tx->rts_rate << 24 : 0));
	rtw88_put32(dst + 24, tx->report_id);
	if (!tx->raw)
		rtw88_put32(dst + 36, (uint32_t)(rtw88_get16(frame + 22) >> 4) << 12);
	for (i = 0; i < 32; i += 2)
		check ^= rtw88_get16(dst + i);
	rtw88_put16(dst + 28, check);
	return 0;
}

static int8_t
rx_signal(const uint8_t *phy, size_t len, uint8_t rfe)
{
	static const int8_t gain0[] = {22, 8, -6, -22, -31, -40, -46, -52};
	static const int8_t gain1[] = {10, 6, 2, -2, -6, -10, -14, -17, -20, -24, -28, -31, -34, -37, -40, -44};
	int strength;
	if (len < 16)
		return -100;
	if ((phy[0] & 15) == 0) {
		unsigned int lna = ((phy[13] >> 5) & 7) | ((phy[14] >> 4) & 8);
		if (rfe == 0 && lna >= sizeof(gain0))
			return -120;
		strength = (rfe == 0 ? gain0[lna] : gain1[lna]) - 2 * (phy[13] & 31);
	} else if ((phy[0] & 15) == 1) {
		strength = (phy[1] & 0x7f) - 110;
	} else {
		return -100;
	}
	if (strength < -127)
		strength = -127;
	if (strength > 0)
		strength = 0;
	return (int8_t)strength;
}

int
rtw88_rx_parse(const uint8_t *buf, size_t len, uint16_t freq, uint8_t rfe,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg)
{
	size_t at = 0;
	if (buf == NULL || input == NULL)
		return EINVAL;
	while (at < len) {
		struct rtw88_rx_packet packet;
		uint32_t w0, w2;
		size_t payload, info, shift, size, rounded;
		int error;
		if (len - at < RTW88_RXDESC)
			return EINVAL;
		w0 = rtw88_get32(buf + at);
		w2 = rtw88_get32(buf + at + 8);
		payload = w0 & 0x3fff;
		info = ((w0 >> 16) & 15) * 8;
		shift = (w0 >> 24) & 3;
		size = RTW88_RXDESC + info + shift;
		if (payload == 0 || payload > len - at || size > len - at - payload)
			return EINVAL;
		memset(&packet, 0, sizeof(packet));
		packet.c2h = (w2 & BIT(28)) != 0;
		packet.len = payload;
		packet.data = buf + at + size;
		packet.freq = freq;
		packet.timestamp = rtw88_get32(buf + at + 20);
		packet.rssi = (w0 & BIT(26)) != 0 ? rx_signal(buf + at + RTW88_RXDESC + shift, info, rfe) : -100;
		packet.decrypted = (w0 & BIT(27)) == 0 && ((w0 >> 20) & 7) == 4;
		if (packet.c2h || ((w0 & (BIT(14) | BIT(15))) == 0 && payload >= 24 && payload <= RTW88_MAX_FRAME)) {
			error = input(arg, &packet);
			if (error != 0)
				return error;
		}
		size += payload;
		rounded = (size + 7) & ~(size_t)7;
		if (rounded >= len - at)
			return 0;
		at += rounded;
	}
	return 0;
}

int
rtw88_tx_report(const uint8_t *data, size_t len, uint8_t *sequence, bool *acked)
{
	if (data == NULL || sequence == NULL || acked == NULL)
		return EINVAL;
	if (len >= 2 && data[0] == 0xff) {
		if (len < 12 || data[2] != 0x0f)
			return ENOENT;
		*sequence = data[10] & 0xfc;
		*acked = (data[11] & 0xc0) == 0;
		return 0;
	}
	if (len < 9 || data[0] != 0x03)
		return ENOENT;
	*sequence = data[8] & 0xfc;
	*acked = (data[2] & 0xc0) == 0;
	return 0;
}
