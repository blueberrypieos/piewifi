/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018-2019 Realtek Corporation
 * Copyright (c) 2021 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 * Copyright (c) 2021 Jernej Skrabec <jernej.skrabec@gmail.com>
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 * Modified 2026-11-24: Native PIE SDIO transport with checked errors and one-block receive reads.
 */
#include <errno.h>
#include <string.h>
#include "rtw88.h"

uint16_t
rtw88_get16(const void *buf)
{
	const uint8_t *p = buf;
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

uint32_t
rtw88_get32(const void *buf)
{
	const uint8_t *p = buf;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void
rtw88_put16(void *buf, uint16_t value)
{
	uint8_t *p = buf;
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

void
rtw88_put32(void *buf, uint32_t value)
{
	uint8_t *p = buf;
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

uint32_t
rtw88_field(uint32_t value, uint32_t mask)
{
	if (mask == 0)
		return 0;
	return (value << __builtin_ctz(mask)) & mask;
}

void
rtw88_fail(struct rtw88_hw *hw, int error)
{
	if (hw->error == 0 && error != 0)
		hw->error = error > 0 ? error : EIO;
}

void
rtw88_delay(struct rtw88_hw *hw, unsigned int usec)
{
	if (hw->error == 0)
		hw->io.delay(hw->io.ctx, usec);
}

static uint8_t
raw_read(struct rtw88_hw *hw, uint32_t addr)
{
	uint8_t value = 0;
	if (hw->error == 0)
		rtw88_fail(hw, hw->io.read_byte(hw->io.ctx, addr, &value));
	return hw->error == 0 ? value : 0;
}

static void
raw_write(struct rtw88_hw *hw, uint32_t addr, uint8_t value)
{
	if (hw->error == 0)
		rtw88_fail(hw, hw->io.write_byte(hw->io.ctx, addr, value));
}

static void
note_failure(struct rtw88_hw *hw, int before, uint32_t addr)
{
	if (before == 0 && hw->error != 0 && hw->failed_register == 0)
		hw->failed_register = addr;
}

static bool
direct(struct rtw88_hw *hw, uint32_t addr)
{
	return (addr & 0xffff0000) != 0 || (hw->powered && !hw->uhs);
}

static uint32_t
bus_address(struct rtw88_hw *hw, uint32_t addr)
{
	if ((addr & 0xffff0000) == 0)
		return 0x10000 | addr;
	if ((addr & 0xffff0000) == RTW88_LOCAL)
		return addr & 0xfff;
	if ((addr & 0xffff0000) == RTW88_MAC)
		return 0x10000 | (addr & 0xffff);
	rtw88_fail(hw, EINVAL);
	return 0;
}

static void
indirect_command(struct rtw88_hw *hw, uint32_t addr, uint32_t flags)
{
	uint32_t command = addr | flags | RTW88_BIT(20);
	unsigned int i;
	if (hw->error != 0)
		return;
	for (i = 0; i < 4; i++)
		raw_write(hw, 0x40 + i, (uint8_t)(command >> (8 * i)));
	for (i = 0; i < 50 && hw->error == 0; i++) {
		if ((raw_read(hw, 0x42) & RTW88_BIT(4)) != 0)
			return;
		rtw88_delay(hw, 1);
	}
	hw->failed_register = addr;
	rtw88_fail(hw, ETIMEDOUT);
}

uint8_t
rtw88_read8(struct rtw88_hw *hw, uint32_t addr)
{
	uint8_t value;
	if (hw->error != 0)
		return 0;
	if (direct(hw, addr)) {
		value = raw_read(hw, bus_address(hw, addr));
	} else {
		indirect_command(hw, addr, RTW88_BIT(19));
		value = raw_read(hw, 0x44);
	}
	note_failure(hw, 0, addr);
	return value;
}

uint16_t
rtw88_read16(struct rtw88_hw *hw, uint32_t addr)
{
	const uint32_t reg = addr;
	const int before = hw->error;
	uint8_t buf[2];
	unsigned int i;
	if (direct(hw, addr)) {
		addr = bus_address(hw, addr);
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = raw_read(hw, addr + i);
	} else if ((addr & 1) != 0) {
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = rtw88_read8(hw, addr + i);
	} else {
		indirect_command(hw, addr, RTW88_BIT(19));
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = raw_read(hw, 0x44 + i);
	}
	note_failure(hw, before, reg);
	return hw->error == 0 ? rtw88_get16(buf) : 0;
}

uint32_t
rtw88_read32(struct rtw88_hw *hw, uint32_t addr)
{
	const uint32_t reg = addr;
	const int before = hw->error;
	uint8_t buf[4] = {0};
	unsigned int i;
	if (direct(hw, addr)) {
		addr = bus_address(hw, addr);
		if (hw->powered && (addr & 3) == 0 && hw->error == 0)
			rtw88_fail(hw, hw->io.read(hw->io.ctx, addr, buf, sizeof(buf), true));
		else
			for (i = 0; i < sizeof(buf); i++)
				buf[i] = raw_read(hw, addr + i);
	} else if ((addr & 3) != 0) {
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = rtw88_read8(hw, addr + i);
	} else {
		indirect_command(hw, addr, RTW88_BIT(19));
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = raw_read(hw, 0x44 + i);
	}
	note_failure(hw, before, reg);
	return hw->error == 0 ? rtw88_get32(buf) : 0;
}

void
rtw88_write8(struct rtw88_hw *hw, uint32_t addr, uint8_t value)
{
	const uint32_t reg = addr;
	const int before = hw->error;
	if (direct(hw, addr)) {
		raw_write(hw, bus_address(hw, addr), value);
		note_failure(hw, before, reg);
		return;
	}
	raw_write(hw, 0x44, value);
	indirect_command(hw, addr, RTW88_BIT(18));
	note_failure(hw, before, reg);
}

void
rtw88_write16(struct rtw88_hw *hw, uint32_t addr, uint16_t value)
{
	const uint32_t reg = addr;
	const int before = hw->error;
	unsigned int i;
	if (direct(hw, addr) || (addr & 1) != 0) {
		addr = bus_address(hw, addr);
		for (i = 0; i < 2; i++)
			raw_write(hw, addr + i, (uint8_t)(value >> (8 * i)));
		note_failure(hw, before, reg);
		return;
	}
	for (i = 0; i < 2; i++)
		raw_write(hw, 0x44 + i, (uint8_t)(value >> (8 * i)));
	indirect_command(hw, addr, RTW88_BIT(18) | RTW88_BIT(16));
	note_failure(hw, before, reg);
}

void
rtw88_write32(struct rtw88_hw *hw, uint32_t addr, uint32_t value)
{
	const uint32_t reg = addr;
	const int before = hw->error;
	uint8_t buf[4];
	unsigned int i;
	rtw88_put32(buf, value);
	if (direct(hw, addr) || (addr & 3) != 0) {
		addr = bus_address(hw, addr);
		if (hw->powered && (addr & 3) == 0 && hw->error == 0)
			rtw88_fail(hw, hw->io.write(hw->io.ctx, addr, buf, sizeof(buf), true));
		else
			for (i = 0; i < sizeof(buf); i++)
				raw_write(hw, addr + i, buf[i]);
		note_failure(hw, before, reg);
		return;
	}
	for (i = 0; i < sizeof(buf); i++)
		raw_write(hw, 0x44 + i, buf[i]);
	indirect_command(hw, addr, RTW88_BIT(18) | RTW88_BIT(17));
	note_failure(hw, before, reg);
}

int
rtw88_irq_mask(struct rtw88_hw *hw, uint32_t mask)
{
	uint8_t buf[4];
	uint32_t addr = RTW88_SDIO_HIMR & 0xfff;
	unsigned int i;
	int error;
	rtw88_put32(buf, mask);
	if (hw->powered)
		return hw->io.write(hw->io.ctx, addr, buf, sizeof(buf), true);
	for (i = 0; i < sizeof(buf); i++) {
		error = hw->io.write_byte(hw->io.ctx, addr + i, buf[i]);
		if (error != 0)
			return error;
	}
	return 0;
}

void
rtw88_mask8(struct rtw88_hw *hw, uint32_t addr, uint8_t mask, uint8_t value)
{
	uint8_t old = rtw88_read8(hw, addr);
	rtw88_write8(hw, addr, (uint8_t)((old & ~mask) | rtw88_field(value, mask)));
}

void
rtw88_mask16(struct rtw88_hw *hw, uint32_t addr, uint16_t mask, uint16_t value)
{
	uint16_t old = rtw88_read16(hw, addr);
	rtw88_write16(hw, addr, (uint16_t)((old & ~mask) | rtw88_field(value, mask)));
}

void
rtw88_mask32(struct rtw88_hw *hw, uint32_t addr, uint32_t mask, uint32_t value)
{
	uint32_t old = rtw88_read32(hw, addr);
	rtw88_write32(hw, addr, (old & ~mask) | rtw88_field(value, mask));
}

int
rtw88_poll(struct rtw88_hw *hw, uint32_t addr, uint32_t mask, uint32_t value,
    unsigned int width, unsigned int interval_us, unsigned int tries)
{
	unsigned int i;
	uint32_t got;
	if (width != 1 && width != 2 && width != 4)
		return EINVAL;
	for (i = 0; i < tries && hw->error == 0; i++) {
		got = width == 1 ? rtw88_read8(hw, addr) : width == 2 ?
		    rtw88_read16(hw, addr) : rtw88_read32(hw, addr);
		if ((got & mask) == value)
			return hw->error;
		rtw88_delay(hw, interval_us);
	}
	if (hw->error == 0)
		hw->failed_register = addr;
	return hw->error != 0 ? hw->error : ETIMEDOUT;
}

size_t
rtw88_align(const struct rtw88_hw *hw, size_t len)
{
	size_t align = len > hw->io.block_size ? hw->io.block_size : 4;
	if (align < 4 || (align & (align - 1)) != 0 || len > SIZE_MAX - align + 1)
		return 0;
	return (len + align - 1) & ~(align - 1);
}

int
rtw88_read_port(struct rtw88_hw *hw, void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t chunk, limit;
	uint32_t addr = 0xe000 | (hw->rx_addr++ & 3);
	int error = 0, next;
	if (len == 0 || hw->io.max_request == 0 || hw->io.block_size == 0)
		return EINVAL;
	limit = hw->io.max_request < hw->io.block_size ? hw->io.max_request : hw->io.block_size;
	while (len != 0) {
		chunk = len < limit ? len : limit;
		next = hw->io.read(hw->io.ctx, addr, p, chunk, true);
		if (error == 0)
			error = next;
		p += chunk;
		len -= chunk;
	}
	return error;
}

int
rtw88_tx_free(struct rtw88_hw *hw, uint32_t pages[3])
{
	unsigned int i;
	for (i = 0; i < 3; i++)
		pages[i] = rtw88_read32(hw, RTW88_LOCAL + 0x20 + 4 * (uint32_t)i) & UINT32_C(0x0fff0fff);
	return hw->error;
}

int
rtw88_write_port(struct rtw88_hw *hw, uint8_t qsel, const void *buf,
    size_t len, bool check_space)
{
	uint32_t pages[3], free_pages, dedicated = 0, needed = 0, address;
	unsigned int field = 0;
	size_t padded = rtw88_align(hw, len);
	int error;
	if (hw->error != 0)
		return hw->error;
	if (len == 0 || padded == 0 || padded > RTW88_RX_BUFFER || padded > hw->io.max_request)
		return EMSGSIZE;
	if (qsel == 16 || qsel == 17 || qsel == 19) {
		address = 4 << 13;
	} else if (qsel == 18) {
		address = 7 << 13;
	} else if (qsel == 4 || qsel == 5 || qsel == 6 || qsel == 7) {
		address = 5 << 13;
	} else if (qsel < 16) {
		address = 6 << 13;
	} else {
		return EINVAL;
	}
	if (check_space) {
		pages[0] = rtw88_read32(hw, RTW88_LOCAL + 0x20);
		pages[1] = rtw88_read32(hw, RTW88_LOCAL + 0x24);
		pages[2] = rtw88_read32(hw, RTW88_LOCAL + 0x28);
		if (hw->error != 0)
			return hw->error;
		field = qsel == 16 || qsel == 17 || qsel == 19 ? 0 : qsel == 18 ? 4 :
		    address == (5 << 13) ? 1 : 2;
		dedicated = (pages[field / 2] >> (16 * (field % 2))) & 0xfff;
		free_pages = dedicated + ((pages[1] >> 16) & 0xfff);
		needed = (uint32_t)((padded + 127) / 128);
		if (needed > free_pages)
			return EAGAIN;
	}
	address |= (uint32_t)((len + 3) / 4);
	memmove(hw->scratch, buf, len);
	memset(hw->scratch + len, 0, padded - len);
	error = hw->io.write(hw->io.ctx, address, hw->scratch, padded, true);
	if (error == 0 && check_space) {
		hw->tx_used[field] += (uint16_t)(needed < dedicated ? needed : dedicated);
		hw->tx_used[3] += (uint16_t)(needed > dedicated ? needed - dedicated : 0);
	}
	return error;
}
