/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtw88.h"
#include "rtw88_registers.h"
#include "table_compat/main.h"

struct fake {
	uint8_t bus[0x20000];
	unsigned int reads, writes, calls, fail_at, delays, indirect_commands;
	int fail_code;
	unsigned int rf_window_writes;
	uint32_t last_addr;
	size_t last_len;
	uint8_t port[RTW88_RX_BUFFER];
	size_t port_cursor, read_limit;
	uint32_t port_addr;
	unsigned int port_reads;
	bool stream_port;
	bool drain;
	bool consume_fifo;
	bool stuck_rx_enable;
	bool stuck_indirect;
	bool stuck_cam;
	unsigned int txdma_clears;
	uint32_t lte[64];
};

static int
fault(struct fake *fake)
{
	fake->calls++;
	return fake->fail_at != 0 && fake->calls >= fake->fail_at ? fake->fail_code : 0;
}

static void
register_command(struct fake *fake, uint32_t addr)
{
	uint32_t command, offset;
	if (addr == 0x10c90) {
		command = rtw88_get32(fake->bus + addr);
		offset = (command >> 20) & 0xff;
		rtw88_put32(fake->bus + 0x12800 + offset * 4, command & 0xfffff);
		return;
	}
	if (addr != 0x11700)
		return;
	command = rtw88_get32(fake->bus + addr);
	offset = command & 0xffff;
	assert(offset < 64);
	if ((command & 0xc0000000U) == 0x80000000U)
		rtw88_put32(fake->bus + 0x11708, fake->lte[offset]);
	if ((command & 0xc0000000U) == 0xc0000000U)
		fake->lte[offset] = rtw88_get32(fake->bus + 0x11704);
	rtw88_put32(fake->bus + addr, command | LTECOEX_READY);
}

static int
read_byte(void *ctx, uint32_t addr, uint8_t *value)
{
	struct fake *fake = ctx;
	int error = fault(fake);
	assert(addr < sizeof(fake->bus));
	fake->reads++;
	if (error == 0)
		*value = fake->bus[addr];
	return error;
}

static int
write_byte(void *ctx, uint32_t addr, uint8_t value)
{
	struct fake *fake = ctx;
	uint32_t command, target, width;
	int error = fault(fake);
	assert(addr < sizeof(fake->bus));
	fake->writes++;
	if (error != 0)
		return error;
	fake->bus[addr] = value;
	if (addr == 0x10100 && fake->stuck_rx_enable)
		fake->bus[addr] |= BIT_MACRXEN;
	if (addr == 0x43) {
		fake->indirect_commands++;
		command = rtw88_get32(fake->bus + 0x40);
		target = 0x10000 | (command & 0xffff);
		width = (command & BIT(17)) != 0 ? 4 : (command & BIT(16)) != 0 ? 2 : 1;
		assert(target + 4 <= sizeof(fake->bus));
		if ((command & BIT(18)) != 0) {
			if (target >= 0x12800 && target < 0x12c00) {
				fake->rf_window_writes++;
			} else {
				memcpy(fake->bus + target, fake->bus + 0x44, width);
				register_command(fake,target);
			}
		}
		if ((command & BIT(19)) != 0)
			memcpy(fake->bus + 0x44, fake->bus + target, 4);
		if (fake->stuck_indirect)
			fake->bus[0x42] &= (uint8_t)~BIT(4);
	}
	if (addr == 0x10673 && !fake->stuck_cam)
		fake->bus[addr] &= 0x7f;
	return 0;
}

static int
read_bytes(void *ctx, uint32_t addr, void *buf, size_t len, bool incr)
{
	struct fake *fake = ctx;
	int error = fault(fake);
	assert(incr);
	fake->last_addr = addr;
	fake->last_len = len;
	fake->reads++;
	if (error != 0)
		return error;
	if ((addr & 0x1e000) == 0xe000) {
		if (fake->stream_port) {
			if (fake->port_reads == 0)
				fake->port_addr = addr;
			assert(addr == fake->port_addr && len <= fake->read_limit);
			assert(fake->port_cursor + len <= sizeof(fake->port));
			memcpy(buf, fake->port + fake->port_cursor, len);
			fake->port_cursor += len;
			fake->port_reads++;
		} else {
			memcpy(buf, fake->port, len);
		}
		if (fake->consume_fifo)
			rtw88_put32(fake->bus + 0x1c,0);
		return 0;
	}
	assert(addr + len <= sizeof(fake->bus));
	memcpy(buf, fake->bus + addr, len);
	return 0;
}

static int
write_bytes(void *ctx, uint32_t addr, const void *buf, size_t len, bool incr)
{
	struct fake *fake = ctx;
	int error = fault(fake);
	assert(incr);
	fake->last_addr = addr;
	fake->last_len = len;
	fake->writes++;
	if (error != 0)
		return error;
	if (addr >= 0x8000 && addr < 0x10000) {
		assert(len <= sizeof(fake->port));
		memcpy(fake->port, buf, len);
	} else {
		assert(addr + len <= sizeof(fake->bus));
		if (addr == 0x18 && len == 4) {
			rtw88_put32(fake->bus + addr, rtw88_get32(fake->bus + addr) & ~rtw88_get32(buf));
			return 0;
		}
		if (addr >= 0x12800 && addr < 0x12c00) {
			fake->rf_window_writes++;
			return 0;
		}
		memcpy(fake->bus + addr, buf, len);
		register_command(fake,addr);
		if (addr == 0x10670 && !fake->stuck_cam)
			fake->bus[addr + 3] &= 0x7f;
		if (addr == 0x10210)
			fake->txdma_clears++;
	}
	return 0;
}

static void
delay(void *ctx, unsigned int usec)
{
	struct fake *fake = ctx;
	assert(usec <= 50000);
	fake->delays++;
}

static void
setup(struct rtw88_hw *hw, struct fake *fake)
{
	memset(hw, 0, sizeof(*hw));
	memset(fake, 0, sizeof(*fake));
	hw->io = (struct rtw88_io){read_byte, write_byte, read_bytes, write_bytes, delay, fake, 4096, 512};
	fake->fail_code = EIO;
}

static void
test_sdio(struct rtw88_hw *hw, struct fake *fake)
{
	unsigned int calls;
	setup(hw, fake);
	rtw88_write32(hw, 0x400, 0x12345678);
	assert(hw->error == 0 && rtw88_get32(fake->bus + 0x10400) == 0x12345678);
	assert(rtw88_read32(hw, 0x400) == 0x12345678);
	hw->powered = true;
	rtw88_write32(hw, 0x404, 0x11223344);
	assert(rtw88_read32(hw, 0x404) == 0x11223344);
	hw->uhs = true;
	rtw88_write16(hw, 0x408, 0x8899);
	assert(rtw88_read16(hw, 0x408) == 0x8899);
	rtw88_write32(hw, 0x40d, 0x44332211);
	assert(rtw88_read32(hw, 0x40d) == 0x44332211);
	assert(rtw88_align(hw, 513) == 1024);
	assert(rtw88_align(hw, 511) == 512);
	assert(rtw88_align(hw, SIZE_MAX) == 0);
	fake->fail_at = fake->calls + 1;
	assert(rtw88_read8(hw, 0x400) == 0 && hw->error == EIO);
	calls = fake->calls;
	rtw88_mask32(hw, 0x400, 0xff, 9);
	assert(fake->calls == calls);
	setup(hw, fake);
	assert(rtw88_poll(hw, RTW88_LOCAL, 1, 1, 1, 1, 3) == ETIMEDOUT);
	assert(fake->reads == 3 && fake->delays == 3);
	assert(rtw88_poll(hw, RTW88_LOCAL, 1, 1, 3, 1, 3) == EINVAL);
	setup(hw, fake);
	fake->stuck_indirect = true;
	fake->bus[0x86] = BIT_HCI_SUS_REQ | BIT_HCI_RESUME_RDY;
	assert(rtw88_power(hw, true) == ETIMEDOUT);
	assert(fake->bus[0x86] == BIT_HCI_RESUME_RDY && fake->indirect_commands == 1);
	assert(hw->failed_register == REG_SYS_CFG1);
	calls = fake->calls;
	rtw88_write8(hw, REG_RSV_CTRL, 0);
	assert(hw->failed_register == REG_SYS_CFG1 && fake->calls == calls);
	setup(hw, fake);
	fake->fail_at = 1;
	assert(rtw88_read32(hw, REG_SYS_CFG1) == 0 && hw->error == EIO);
	rtw88_write8(hw, REG_RSV_CTRL, 0);
	assert(hw->failed_register == REG_SYS_CFG1 && fake->calls == 1);
	setup(hw, fake);
	assert(rtw88_power(hw, true) == ETIMEDOUT);
	assert(hw->failed_register == REG_SDIO_HSUS_CTRL && fake->indirect_commands == 0);
	setup(hw, fake);
	fake->fail_at = 1;
	assert(rtw88_power(hw, true) == EIO && fake->calls == 1);
	assert(fake->indirect_commands == 0);
	setup(hw, fake);
	hw->powered = true;
	fake->fail_at = 1;
	assert(rtw88_read32(hw, RTW88_LOCAL + 0x1c) == 0 && hw->error == EIO);
	assert(hw->failed_register == RTW88_LOCAL + 0x1c);
	setup(hw, fake);
	hw->powered = true;
	fake->fail_at = 2;
	rtw88_write16(hw, 0x0522, 0xffff);
	assert(hw->error == EIO && hw->failed_register == 0x0522);
}

static void
test_irq_mask(struct rtw88_hw *hw, struct fake *fake)
{
	for (unsigned int powered = 0; powered < 2; powered++) {
		for (unsigned int uhs = 0; uhs < 2; uhs++) {
			setup(hw,fake);
			hw->powered = powered != 0;
			hw->uhs = uhs != 0;
			hw->error = ETIMEDOUT;
			hw->failed_register = REG_SYS_CFG1;
			assert(rtw88_irq_mask(hw,0x12345678) == 0);
			assert(rtw88_get32(fake->bus + 0x14) == 0x12345678);
			assert(fake->calls == (powered ? 1U : 4U));
			if (powered)
				assert(fake->last_addr == 0x14 && fake->last_len == 4);
			assert(hw->error == ETIMEDOUT && hw->failed_register == REG_SYS_CFG1);
			assert(rtw88_irq_mask(hw,0) == 0 && rtw88_get32(fake->bus + 0x14) == 0);
			fake->fail_at = fake->calls + 1;
			assert(rtw88_irq_mask(hw,RTW88_IRQ_MASK) == EIO);
			assert(hw->error == ETIMEDOUT && hw->failed_register == REG_SYS_CFG1);
			fake->fail_at = 0;
			hw->error = 0;
			hw->failed_register = 0;
			fake->fail_at = fake->calls + 1;
			assert(rtw88_irq_mask(hw,RTW88_IRQ_MASK) == EIO);
			assert(hw->error == 0 && hw->failed_register == 0);
		}
	}
}

static void
test_ports(struct rtw88_hw *hw, struct fake *fake)
{
	uint8_t buffer[1024];
	unsigned int calls;
	setup(hw, fake);
	memset(buffer, 0x5a, sizeof(buffer));
	assert(rtw88_write_port(hw, 0, buffer, 513, false) == 0);
	assert(fake->last_addr == (0xc000 | 129) && fake->last_len == 1024);
	assert(fake->port[512] == 0x5a && fake->port[513] == 0 && fake->port[1023] == 0);
	assert(rtw88_write_port(hw, 0, buffer, 16385, false) == EMSGSIZE);
	assert(rtw88_write_port(hw, 20, buffer, 1, false) == EINVAL);
	assert(rtw88_write_port(hw, 18, buffer, 513, true) == EAGAIN);
	fake->bus[0x28] = 8;
	assert(rtw88_write_port(hw, 18, buffer, 513, true) == 0);
	assert(hw->tx_used[4] == 8 && hw->tx_used[3] == 0);
	fake->bus[0x28] = 3;
	fake->bus[0x26] = 9;
	assert(rtw88_write_port(hw, 18, buffer, 513, true) == 0);
	assert(hw->tx_used[4] == 11 && hw->tx_used[3] == 5);
	memset(hw->tx_used, 0, sizeof(hw->tx_used));
	assert(rtw88_write_port(hw, 18, buffer, 513, false) == 0 && hw->tx_used[4] == 0);
	hw->io.max_request = 512;
	assert(rtw88_write_port(hw, 0, buffer, 513, false) == EMSGSIZE);
	hw->io.max_request = 4096;
	hw->error = EIO;
	calls = fake->calls;
	assert(rtw88_write_port(hw, 0, buffer, 4, false) == EIO && fake->calls == calls);
	hw->error = 0;
	{
		uint32_t pages[3];
		rtw88_put32(fake->bus + 0x20, 0xf0100010);
		rtw88_put32(fake->bus + 0x24, 0x018d0010);
		rtw88_put32(fake->bus + 0x28, 0x2020000e);
		assert(rtw88_tx_free(hw, pages) == 0);
		assert(pages[0] == 0x00100010 && pages[1] == 0x018d0010 && pages[2] == 0x0020000e);
		memset(fake->bus + 0x20, 0, 12);
	}
	hw->io.max_request = 512;
	assert(rtw88_read_port(hw, buffer, sizeof(buffer)) == 0);
	assert(fake->last_len == 512 && fake->last_addr == 0xe000);
	fake->fail_at = fake->calls + 1;
	assert(rtw88_read_port(hw, buffer, sizeof(buffer)) == EIO);
	assert(fake->last_addr == 0xe001);
}

static void
test_rx_chunks(struct rtw88_hw *hw, struct fake *fake)
{
	static const size_t lengths[] = {1,511,512,513,1024,1536,RTW88_RX_BUFFER};
	static const size_t requests[] = {256,512,4096};
	uint8_t buffer[RTW88_RX_BUFFER + 2];
	for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
		for (size_t j = 0; j < sizeof(lengths) / sizeof(lengths[0]); j++) {
			setup(hw,fake);
			hw->io.max_request = requests[i];
			fake->read_limit = requests[i] < 512 ? requests[i] : 512;
			fake->stream_port = true;
			for (size_t k = 0; k < sizeof(fake->port); k++)
				fake->port[k] = (uint8_t)((k * 17) ^ (k >> 8));
			memset(buffer,0xa5,sizeof(buffer));
			assert(rtw88_read_port(hw,buffer + 1,lengths[j]) == 0);
			assert(fake->port_cursor == lengths[j] && fake->port_reads != 0);
			assert(fake->port_addr == 0xe000 && hw->rx_addr == 1);
			assert(memcmp(buffer + 1,fake->port,lengths[j]) == 0);
			assert(buffer[0] == 0xa5 && buffer[lengths[j] + 1] == 0xa5);
			fake->port_reads = 0;
			fake->port_cursor = 0;
			assert(rtw88_read_port(hw,buffer + 1,lengths[j]) == 0);
			assert(fake->port_addr == 0xe001 && hw->rx_addr == 2);
		}
	}
	setup(hw,fake);
	hw->io.max_request = 0;
	assert(rtw88_read_port(hw,buffer,1) == EINVAL && fake->calls == 0);
	setup(hw,fake);
	hw->io.block_size = 0;
	assert(rtw88_read_port(hw,buffer,1) == EINVAL && fake->calls == 0);
}

static void
test_efuse(void)
{
	uint8_t data[16] = {0x1e, 0x55, 0x66, 0xff};
	uint8_t logical[512];
	assert(rtw88_decode_efuse(data, 4, logical, sizeof(logical)) == 0);
	assert(logical[8] == 0x55 && logical[9] == 0x66 && logical[10] == 0xff);
	assert(rtw88_decode_efuse(data, 2, logical, sizeof(logical)) == EINVAL);
	data[0] = 0x0f;
	assert(rtw88_decode_efuse(data, 1, logical, sizeof(logical)) == EINVAL);
	data[1] = 0xfe;
	assert(rtw88_decode_efuse(data, 4, logical, 16) == EINVAL);
	data[0] = 0xff;
	assert(rtw88_decode_efuse(data, 1, logical, sizeof(logical)) == 0);
	assert(logical[511] == 0xff);
}

static void
test_firmware(void)
{
	uint8_t data[112] = {0};
	struct rtw88_firmware fw;
	rtw88_put16(data + 4, 24);
	rtw88_put32(data + 0x24, 8);
	rtw88_put32(data + 0x30, 16);
	rtw88_put32(data + 0x20, 0x80200000);
	rtw88_put32(data + 0x3c, 0x80030000);
	assert(rtw88_parse_firmware(data, 104, &fw) == 0);
	assert(fw.count == 2 && fw.version == 24 && fw.segment[0].size == 16 && fw.segment[1].size == 24);
	assert(fw.segment[0].destination == 0x200000 && fw.segment[1].destination == 0x30000);
	assert(rtw88_parse_firmware(data, 103, &fw) == EINVAL);
	assert(rtw88_parse_firmware(data, 105, &fw) == EINVAL);
	assert(rtw88_parse_firmware(data, 63, &fw) == EINVAL);
	rtw88_put32(data + 0x24, UINT32_MAX);
	assert(rtw88_parse_firmware(data, 104, &fw) == EINVAL);
}

static int
input(void *ctx, const struct rtw88_rx_packet *packet)
{
	unsigned int *count = ctx;
	assert(packet->len == 24 && packet->freq == 2412 && !packet->c2h);
	(*count)++;
	return 0;
}

static int
input_signal(void *ctx, const struct rtw88_rx_packet *packet)
{
	unsigned int *count = ctx;
	assert(packet->rssi == -40 && packet->len == 24);
	(*count)++;
	return 0;
}

static void
test_packets(void)
{
	uint8_t buffer[256] = {0}, descriptor[48], frame[32] = {0x08,0x01};
	struct rtw88_tx_packet tx = {.qsel = 18, .rate = 4, .report = true, .report_id = 0x14};
	uint8_t report[12] = {0x03, 0};
	uint8_t sequence = 0;
	bool acked = false;
	unsigned int count = 0;
	uint16_t check = 0;
	size_t at;
	frame[4] = 1;
	rtw88_put16(frame + 22, 0x1230);
	assert(rtw88_tx_descriptor(descriptor, sizeof(descriptor), frame, sizeof(frame), &tx) == 0);
	assert((rtw88_get32(descriptor) & BIT(24)) != 0);
	assert((rtw88_get32(descriptor + 4) & GENMASK(12, 8)) == 18 << 8);
	assert(rtw88_get32(descriptor + 24) == 0x14);
	for (at = 0; at < 32; at += 2)
		check ^= rtw88_get16(descriptor + at);
	assert(check == 0);
	assert(rtw88_tx_descriptor(descriptor, 47, frame, sizeof(frame), &tx) == EINVAL);
	rtw88_put32(buffer, 24);
	rtw88_put32(buffer + 48, 24);
	assert(rtw88_rx_parse(buffer, 96, 2412, 0, input, &count) == 0 && count == 2);
	assert(rtw88_rx_parse(buffer, 95, 2412, 0, input, &count) == EINVAL);
	rtw88_put32(buffer, 0x3fff);
	assert(rtw88_rx_parse(buffer, 48, 2412, 0, input, &count) == EINVAL);
	rtw88_put32(buffer, 24 | BIT(14));
	count = 0;
	assert(rtw88_rx_parse(buffer, 48, 2412, 0, input, &count) == 0 && count == 0);
	report[8] = 0x17;
	assert(rtw88_tx_report(report, 9, &sequence, &acked) == 0 && sequence == 0x14 && acked);
	report[2] = 0x40;
	assert(rtw88_tx_report(report, 9, &sequence, &acked) == 0 && !acked);
	report[0] = 0xff;
	report[2] = 0x0f;
	report[10] = 0x28;
	report[11] = 0;
	assert(rtw88_tx_report(report, 12, &sequence, &acked) == 0 && sequence == 0x28 && acked);
	assert(rtw88_tx_report(report, 11, &sequence, &acked) == ENOENT);
	memset(buffer,0,sizeof(buffer));
	rtw88_put32(buffer,24 | 2U << 16 | 2U << 24 | BIT(26));
	buffer[26] = 1;
	buffer[27] = 70;
	count = 0;
	assert(rtw88_rx_parse(buffer,66,2412,0,input_signal,&count) == 0 && count == 1);
	tx.protection = true;
	assert(rtw88_tx_descriptor(descriptor,sizeof(descriptor),frame,sizeof(frame),&tx) == 0);
	assert((rtw88_get32(descriptor + 12) & RTW_TX_DESC_W3_USE_RTS) != 0);
}

static void
test_keys(struct rtw88_hw *hw, struct fake *fake)
{
	uint8_t key[16] = {0}, addr[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
	setup(hw, fake);
	hw->powered = true;
	assert(rtw88_key(hw, addr, key) == 0 && hw->key_valid);
	assert((rtw88_get32(fake->bus + 0x10674) & 0xffff) == ((4 << 2) | BIT(15)));
	assert(rtw88_key_clear(hw) == 0 && !hw->key_valid);
	fake->fail_at = fake->calls + 1;
	assert(rtw88_key(hw, addr, key) == EIO && !hw->key_valid);
}

static void
test_tables(struct rtw88_hw *hw, struct fake *fake)
{
	const uint32_t words[] = {0x80000402,0,0x40000000,0,0x500,0x12,0xa0000000,0,0x500,0x34,0xb0000000,0};
	const struct rtw_table table = {words, sizeof(words) / sizeof(words[0]), 0};
	setup(hw, fake);
	hw->powered = true;
	hw->rfe = 2;
	assert(rtw88_table(hw, &table) == 0 && fake->bus[0x10500] == 0x12);
	hw->rfe = 0;
	assert(rtw88_table(hw, &table) == 0 && fake->bus[0x10500] == 0x34);
}

static void
test_coex(struct rtw88_hw *hw, struct fake *fake)
{
	uint32_t value = 0;
	setup(hw,fake);
	hw->powered = true;
	rtw88_put32(fake->bus + 0x11700,LTECOEX_READY);
	fake->lte[0x38] = 0xabcd1234;
	assert(rtw88_lte_read(hw,0x38,&value) == 0 && value == 0xabcd1234);
	assert(rtw88_wifi_only(hw) == 0);
	assert(fake->lte[0x38] == 0xabcd7734);
	assert((fake->bus[0x10073] & (BIT_LTE_MUX_CTRL_PATH >> 24)) != 0);
	assert((rtw88_get32(fake->bus + 0x10cb4) & 0xf00000ff) == 0x20000077);
	assert((fake->bus[0x10067] & 0x30) == 0x30);
	assert(rtw88_lte_write(hw,0x38,0x12345678) == 0 && fake->lte[0x38] == 0x12345678);
	setup(hw,fake);
	hw->powered = true;
	assert(rtw88_lte_read(hw,0x38,&value) == ETIMEDOUT);
	assert(fake->writes == 0 && fake->delays == 1000);
	setup(hw,fake);
	hw->powered = true;
	hw->rfe = 6;
	rtw88_put32(fake->bus + 0x11700,LTECOEX_READY);
	assert(rtw88_wifi_only(hw) == 0 && rtw88_get32(fake->bus + 0x10cb4) == 0);
}

static void
test_h2c(struct rtw88_hw *hw, struct fake *fake)
{
	uint8_t general[] = {0,0,48,0};
	uint8_t iqk = 0;
	uint8_t expected[80] = {0x20,0,0,0,0,0x13};
	setup(hw,fake);
	hw->powered = hw->firmware_ready = true;
	fake->bus[0x20] = 16;
	expected[28] = 0x20;
	expected[29] = 0x13;
	expected[48] = 1;
	expected[49] = 0xff;
	expected[50] = 0x0d;
	expected[52] = 12;
	expected[58] = 48;
	assert(rtw88_h2c(hw,H2C_PKT_GENERAL_INFO,general,sizeof(general)) == 0);
	assert(fake->last_len == sizeof(expected) && memcmp(fake->port,expected,sizeof(expected)) == 0);
	expected[50] = 0x0e;
	expected[52] = 9;
	expected[54] = 1;
	expected[58] = 0;
	assert(rtw88_h2c(hw,H2C_PKT_IQK,&iqk,1) == 0);
	assert(memcmp(fake->port,expected,sizeof(expected)) == 0);
}

static void
test_link_timing(struct rtw88_hw *hw, struct fake *fake)
{
	uint8_t bssid[6] = {0};
	unsigned int writes;
	setup(hw,fake);
	hw->powered = true;
	hw->freq = 5745;
	assert(rtw88_state(hw,4,bssid,1,100,1,0x51) == 0);
	assert((rtw88_get32(fake->bus + 0x10440) & 0xfffff) == 0x50);
	assert(rtw88_edca(hw,0,3,4,10,0) == 0 && fake->bus[0x10508] == 43);
	hw->freq = 2412;
	assert(rtw88_state(hw,4,bssid,1,100,4,0) == 0 && hw->slot_time == 20 && hw->protection);
	assert(rtw88_edca(hw,0,3,4,10,0) == 0 && fake->bus[0x10508] == 70);
	writes = fake->writes;
	assert(rtw88_edca(hw,0,15,4,10,0) == EINVAL && fake->writes == writes);
	rtw88_put16(fake->bus + 0x10230,16);
	fake->bus[0x10522] = 0;
	writes = fake->delays;
	assert(rtw88_tx_quiesce(hw) == 0 && fake->bus[0x10522] == 0xff && fake->delays == writes + 5);
	rtw88_put16(fake->bus + 0x10232,16);
	fake->bus[0x10522] = 0;
	writes = fake->delays;
	assert(rtw88_tx_quiesce(hw) == 0 && fake->bus[0x10522] == 0xff && fake->delays == writes);
	bssid[0] = 0x02;
	hw->key_valid = true;
	fake->stuck_cam = true;
	fake->bus[0x10618] = 0;
	fake->bus[0x10522] = 0xff;
	assert(rtw88_state(hw,2,bssid,0,100,0,0) == ETIMEDOUT && hw->error == 0);
	assert(!hw->key_valid && fake->bus[0x10618] == 0 && fake->bus[0x10522] == 0xff);
	fake->stuck_cam = false;
	assert(rtw88_state(hw,2,bssid,0,100,0,0) == 0 && fake->bus[0x10618] == 0x02 && fake->bus[0x10522] == 0);
}

static const struct rtw88_board_profile synthetic_board = {
	.supported_rfe_mask = 0x55,
	.band_2g = {true,300,2300},
	.band_5g = {true,500,1700}
};

static void
test_policy(void)
{
	struct rtw88_board_profile profile = synthetic_board;
	int16_t conducted = 123;
	uint8_t selected = 99;
	uint8_t rates[] = {0x96,12,0x82,108};
	uint8_t five[] = {0x82,0x8b,0x98,0x8c};
	uint8_t cck[] = {0x82,0x84,0x8b};
	const uint8_t countries[][2] = {{0,0},{0xff,0xff},{'0','0'},{'A','U'}};
	const uint8_t corrupt[][2] = {{'A',0xff},{0,'U'},{'a','u'},{0xff,0},{'0','U'}};
	char country[2];
	assert(rtw88_board_limit(NULL,2412,2000,&conducted) == EOPNOTSUPP && conducted == 123);
	assert(rtw88_board_limit(&profile,2412,2000,&conducted) == 0 && conducted == 1700);
	assert(rtw88_board_limit(&profile,5745,3000,&conducted) == 0 && conducted == 1700);
	assert(rtw88_board_limit(&profile,5745,1000,&conducted) == 0 && conducted == 500);
	profile.band_2g.max_net_antenna_gain_mbi = -300;
	assert(rtw88_board_limit(&profile,2412,2000,&conducted) == 0 && conducted == 2000);
	profile.band_5g.supported = false;
	conducted = 123;
	assert(rtw88_board_limit(&profile,5745,2000,&conducted) == EOPNOTSUPP && conducted == 123);
	profile.supported_rfe_mask = 2;
	assert(rtw88_board_validate(&profile) == EINVAL);
	assert(rtw88_basic_rate(2412,rates,sizeof(rates),&selected) == 0 && selected == 0);
	assert(rtw88_basic_rate(5745,five,sizeof(five),&selected) == 0 && selected == 4);
	selected = 99;
	assert(rtw88_basic_rate(5745,cck,sizeof(cck),&selected) == EOPNOTSUPP && selected == 99);
	rates[0] = 108; rates[1] = 24;
	assert(rtw88_basic_rate(2412,rates,2,&selected) == 0 && selected == 6);
	for (unsigned int i = 0; i < sizeof(countries) / sizeof(countries[0]); i++) {
		assert(rtw88_country_decode(countries[i],country) == 0);
		assert(country[0] == (i == 3 ? 'A' : 0));
	}
	for (unsigned int i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
		country[0] = 'X'; country[1] = 'Y';
		assert(rtw88_country_decode(corrupt[i],country) == EILSEQ && country[0] == 'X' && country[1] == 'Y');
	}
}

static void
power_setup(struct rtw88_hw *hw, struct fake *fake, uint8_t base)
{
	setup(hw,fake);
	hw->powered = true;
	hw->board = &synthetic_board;
	for (unsigned int i = 0; i < 11; i++)
		hw->calibration[0x10 + i] = base;
	for (unsigned int i = 0; i < 14; i++)
		hw->calibration[0x10 + 18 + i] = base;
}

static void
test_rf_writes(struct rtw88_hw *hw, struct fake *fake)
{
	unsigned int writes;
	setup(hw,fake);
	hw->powered = true;
	rtw88_rf_write(hw,0x18,0xfffff,0xc06);
	assert(hw->error == 0 && rtw88_rf_read(hw,0x18) == 0xc06);
	assert(rtw88_get32(fake->bus + 0x10c90) == 0x01800c06);
	rtw88_rf_write(hw,0x18,0xff,11);
	assert(rtw88_rf_read(hw,0x18) == 0xc0b);
	assert(rtw88_get32(fake->bus + 0x10c90) == 0x01800c0b);
	rtw88_rf_write(hw,0,0xfffff,0x30000);
	assert(rtw88_rf_read(hw,0) == 0x30000 && fake->rf_window_writes == 0);
	rtw88_put32(fake->bus + 0x12860,0xfffff);
	writes = fake->writes;
	rtw88_rf_write(hw,0x18,0xff,1);
	assert(hw->error == EIO && fake->writes == writes);
}

static void
test_power_programming(struct rtw88_hw *hw, struct fake *fake)
{
	struct rtw88_board_profile high = synthetic_board;
	setup(hw,fake);
	hw->powered = true;
	assert(rtw88_phy_channel(hw,2412,800) == EOPNOTSUPP && fake->writes == 0);
	power_setup(hw,fake,40);
	assert(rtw88_phy_channel(hw,2412,800) == 0);
	assert(rtw88_get32(fake->bus + 0x11d00) == 0x12121212);
	assert(rtw88_get32(fake->bus + 0x11d04) == 0x16161616);
	assert(rtw88_phy_channel(hw,2412,849) == 0 && rtw88_get32(fake->bus + 0x11d00) == 0x12121212);
	assert(rtw88_phy_channel(hw,2412,850) == 0 && rtw88_get32(fake->bus + 0x11d00) == 0x13131313);
	assert(rtw88_phy_channel(hw,2412,300) == 0 && rtw88_get32(fake->bus + 0x11d00) == 0x08080808);
	power_setup(hw,fake,40);
	assert(rtw88_phy_channel(hw,5745,1000) == 0 && rtw88_get32(fake->bus + 0x11d04) == 0x18181818);
	power_setup(hw,fake,0xff);
	assert(rtw88_phy_channel(hw,2412,800) == EOPNOTSUPP && fake->writes == 0);
	power_setup(hw,fake,0);
	assert(rtw88_phy_channel(hw,2412,800) == EOPNOTSUPP && fake->writes == 0);
	power_setup(hw,fake,63);
	high.band_2g.max_net_antenna_gain_mbi = 0;
	high.band_2g.max_conducted_mbm = 3000;
	hw->board = &high;
	assert(rtw88_phy_channel(hw,2412,3000) == 0 && rtw88_get32(fake->bus + 0x11d04) == 0x3f3f3f3f);
}

static void
test_interrupt_status(struct rtw88_hw *hw, struct fake *fake)
{
	static const uint32_t status[] = {BIT(2),BIT(3),BIT(4),BIT(5)};
	unsigned int count = 0;
	setup(hw,fake);
	hw->powered = true;
	rtw88_put32(fake->bus + 0x18,BIT(0) | BIT(4));
	assert(rtw88_rx_drain(hw,input,&count) == 0);
	assert(rtw88_get32(fake->bus + 0x18) == BIT(0) && count == 0);
	for (unsigned int i = 0; i < sizeof(status) / sizeof(status[0]); i++) {
		setup(hw,fake);
		hw->powered = true;
		rtw88_put32(fake->bus + 0x10210,0x40);
		rtw88_put32(fake->bus + 0x18,BIT(0) | status[i]);
		assert(rtw88_rx_drain(hw,input,&count) == 0 && count == 0);
		assert(rtw88_get32(fake->bus + 0x18) == BIT(0));
		assert(fake->txdma_clears == (status[i] == BIT(2) ? 1U : 0U));
	}
	assert(RTW88_IRQ_MASK == BIT(0));
}

static void
fifo_packet(struct fake *fake)
{
	memset(fake->port,0,sizeof(fake->port));
	rtw88_put32(fake->port,24);
	fake->port[RTW88_RXDESC] = 0x08;
	rtw88_put32(fake->bus + 0x1c,RTW88_RXDESC + 24);
}

static void
test_rx_boundary(struct rtw88_hw *hw, struct fake *fake)
{
	unsigned int count = 0;
	setup(hw, fake);
	hw->powered = true;
	hw->freq = 2412;
	fake->bus[0x10100] = 0xff;
	rtw88_put32(fake->bus + 0x10284,BIT_RXDMA_IDLE);
	assert(rtw88_rx_quiesce(hw,input,&count) == 0 && hw->rx_stopped && (fake->bus[0x10100] & BIT_MACRXEN) == 0);
	assert(count == 0 && fake->delays == 0);
	assert(rtw88_rx_resume(hw) == 0 && !hw->rx_stopped && fake->bus[0x10100] == 0xff);
	fake->consume_fifo = true;
	fifo_packet(fake);
	assert(rtw88_rx_quiesce(hw,input,&count) == 0 && rtw88_get32(fake->bus + 0x1c) == 0);
	assert(count == 1 && hw->rx_stopped);
	assert(rtw88_rx_resume(hw) == 0 && fake->bus[0x10100] == 0xff);
	fake->consume_fifo = false;
	fifo_packet(fake);
	memset(fake->port,0,RTW88_RXDESC);
	assert(rtw88_rx_quiesce(hw,input,&count) == EINVAL && hw->rx_stopped);
	assert(rtw88_rx_resume(hw) == 0 && fake->bus[0x10100] == 0xff);
	setup(hw,fake);
	hw->powered = true;
	fake->bus[0x10100] = 0xff;
	assert(rtw88_rx_quiesce(hw,input,&count) == ETIMEDOUT && hw->rx_stopped);
	assert(hw->failed_register == REG_RXPKT_NUM);
	assert((fake->bus[0x10100] & BIT_MACRXEN) == 0 && fake->delays == 1000);
	assert(rtw88_rx_resume(hw) == 0 && !hw->rx_stopped && fake->bus[0x10100] == 0xff);
	setup(hw,fake);
	hw->powered = true;
	fake->stuck_rx_enable = true;
	fake->bus[0x10100] = 0xff;
	assert(rtw88_rx_quiesce(hw,input,&count) == EIO && !hw->rx_stopped);
}

int
main(void)
{
	struct rtw88_hw *hw = calloc(1, sizeof(*hw));
	struct fake *fake = calloc(1, sizeof(*fake));
	assert(hw != NULL && fake != NULL);
	test_sdio(hw, fake);
	test_irq_mask(hw,fake);
	test_ports(hw, fake);
	test_rx_chunks(hw, fake);
	test_efuse();
	test_firmware();
	test_packets();
	test_keys(hw, fake);
	test_tables(hw, fake);
	test_coex(hw, fake);
	test_h2c(hw, fake);
	test_link_timing(hw, fake);
	test_policy();
	test_rf_writes(hw,fake);
	test_power_programming(hw,fake);
	test_interrupt_status(hw,fake);
	test_rx_boundary(hw,fake);
	free(fake);
	free(hw);
	puts("rtw88: transport, bounds, efuse, firmware, descriptors, receive, CAM and table tests passed");
	return 0;
}
