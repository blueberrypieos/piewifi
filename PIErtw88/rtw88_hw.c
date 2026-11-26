/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2018-2019 Realtek Corporation
 * Copyright (c) 2021 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 * Copyright (c) 2021 Jernej Skrabec <jernej.skrabec@gmail.com>
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 * Modified 2026-11-24: Native PIE power, firmware, security and MAC operations with receive drain and quiesce loops.
 */
#include <errno.h>
#include <string.h>
#include "rtw88.h"
#include "rtw88_registers.h"

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define RTW_PWR_DELAY_US 0
#define RTW_PWR_DELAY_MS 1
#define RTW88_FIFO_BOUNDARY 460
#define RTW88_H2C_ADDRESS (500U << 7)
#define RTW88_H2C_SIZE (8U << 7)
struct rtw_pwr_seq_cmd {
	uint16_t offset;
	uint8_t cut_mask, intf_mask, base, cmd, mask, value;
};
#include "rtw88_power.inc"

#define rtw_read8 rtw88_read8
#define rtw_read16 rtw88_read16
#define rtw_read32 rtw88_read32
#define rtw_write8 rtw88_write8
#define rtw_write16 rtw88_write16
#define rtw_write32 rtw88_write32
#define rtw_write8_set(h,a,m) rtw88_write8(h,a,(uint8_t)(rtw88_read8(h,a) | (m)))
#define rtw_write8_clr(h,a,m) rtw88_write8(h,a,(uint8_t)(rtw88_read8(h,a) & ~(m)))
#define rtw_write32_set(h,a,m) rtw88_write32(h,a,rtw88_read32(h,a) | (m))
#define rtw_write32_clr(h,a,m) rtw88_write32(h,a,rtw88_read32(h,a) & ~(m))
#include "rtw88_mac.inc"

static int
power_sequence(struct rtw88_hw *hw, const struct rtw_pwr_seq_cmd * const *seq)
{
	const struct rtw_pwr_seq_cmd *cmd;
	size_t n, at;
	uint32_t addr;
	uint8_t cut = hw->cut < 7 ? (uint8_t)(1U << (hw->cut + 1)) : 0;
	if (cut == 0)
		return ENODEV;
	for (n = 0; seq[n] != NULL; n++) {
		for (at = 0; at < 128; at++) {
			cmd = &seq[n][at];
			if (cmd->cmd == RTW_PWR_CMD_END)
				break;
			if ((cmd->cut_mask & cut) == 0 || (cmd->intf_mask & RTW_PWR_INTF_SDIO_MSK) == 0)
				continue;
			addr = cmd->offset;
			if (cmd->base == RTW_PWR_ADDR_SDIO)
				addr |= RTW88_LOCAL;
			switch (cmd->cmd) {
			case RTW_PWR_CMD_WRITE:
				rtw88_write8(hw, addr, (uint8_t)((rtw88_read8(hw, addr) & ~cmd->mask) | (cmd->value & cmd->mask)));
				break;
			case RTW_PWR_CMD_POLLING:
				if (rtw88_poll(hw, addr, cmd->mask, cmd->value & cmd->mask, 1, 50, 20000) != 0)
					return hw->error != 0 ? hw->error : ETIMEDOUT;
				break;
			case RTW_PWR_CMD_DELAY:
				rtw88_delay(hw, cmd->offset * (cmd->value == RTW_PWR_DELAY_US ? 1U : 1000U));
				break;
			case RTW_PWR_CMD_READ:
				(void)rtw88_read8(hw, addr);
				break;
			default:
				return EINVAL;
			}
			if (hw->error != 0)
				return hw->error;
		}
		if (at == 128)
			return EINVAL;
	}
	return hw->error;
}

static int
resume_sdio(struct rtw88_hw *hw)
{
	rtw88_mask8(hw, REG_SDIO_HSUS_CTRL, BIT_HCI_SUS_REQ, 0);
	return rtw88_poll(hw, REG_SDIO_HSUS_CTRL, BIT_HCI_RESUME_RDY,
	    BIT_HCI_RESUME_RDY, 1, 50, 20000);
}

int
rtw88_power(struct rtw88_hw *hw, bool on)
{
	uint32_t val;
	int error;
	if (!on) {
		rtw88_write32(hw, RTW88_SDIO_HIMR, 0);
		hw->powered = false;
		error = power_sequence(hw, card_disable_flow_8821c);
		hw->firmware_ready = hw->key_valid = false;
		return error;
	}
	error = resume_sdio(hw);
	if (error != 0)
		return error;
	val = rtw88_read32(hw, REG_SYS_CFG1);
	if (hw->error != 0)
		return hw->error;
	hw->cut = (uint8_t)((val >> 12) & 15);
	if ((val & BIT_RF_TYPE_ID) != 0 || hw->cut > 6)
		return ENODEV;
	rtw88_write8(hw, REG_RSV_CTRL, 0);
	error = resume_sdio(hw);
	if (error != 0)
		return error;
	rtw88_mask8(hw, REG_HCI_OPT_CTRL + 2, BIT_SDIO_PAD_E5 >> 16, hw->uhs ? 1 : 0);
	rtw_write32_set(hw, REG_PAD_CTRL1, BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL);
	rtw_write32_clr(hw, REG_LED_CFG, BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN);
	rtw_write32_set(hw, REG_GPIO_MUXCFG, BIT_WLRFE_4_5_EN);
	rtw_write8_clr(hw, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	rtw_write8_clr(hw, REG_RF_CTRL, BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN);
	rtw_write32_clr(hw, REG_WLRF1, BIT_WLRF1_BBRF_EN);
	if (rtw88_read8(hw, REG_CR) != 0xea) {
		hw->powered = false;
		error = power_sequence(hw, card_disable_flow_8821c);
		if (error != 0)
			return error;
	}
	error = power_sequence(hw, card_enable_flow_8821c);
	if (error != 0)
		return error;
	hw->powered = true;
	rtw_write32_set(hw, REG_CPU_DMEM_CON, BIT_WL_PLATFORM_RST | BIT_DDMA_EN);
	rtw_write8_set(hw, REG_SYS_FUNC_EN + 1, 0xd8);
	rtw88_write8(hw, REG_CR_EXT + 3, (uint8_t)((rtw88_read8(hw, REG_CR_EXT + 3) & 0xf0) | 0x0c));
	val = rtw88_read32(hw, REG_MCUFW_CTRL);
	if ((val & BIT_BOOT_FSPI_EN) != 0) {
		rtw88_write32(hw, REG_MCUFW_CTRL, val & ~BIT_BOOT_FSPI_EN);
		rtw_write32_clr(hw, REG_GPIO_MUXCFG, BIT_FSPI_EN);
	}
	return hw->error;
}

int
rtw88_read_efuse(struct rtw88_hw *hw)
{
	uint8_t physical[RTW88_EFUSE_SIZE];
	uint32_t cfg;
	size_t at;
	int error;
	cfg = rtw88_read32(hw, REG_SYS_CFG1);
	hw->cut = (uint8_t)((cfg >> 12) & 15);
	if ((cfg & BIT_RF_TYPE_ID) != 0 || hw->cut > 6)
		return ENODEV;
	rtw88_mask32(hw, REG_LDO_EFUSE_CTRL, BIT_MASK_EFUSE_BANK_SEL, 0);
	rtw_write8_clr(hw, REG_LDO_EFUSE_CTRL + 3, BIT(7));
	for (at = 0; at < sizeof(physical); at++) {
		cfg = rtw88_read32(hw, REG_EFUSE_CTRL);
		cfg = (cfg & ~(BIT_MASK_EF_DATA | BITS_EF_ADDR | BIT_EF_FLAG)) |
		    (((uint32_t)at & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR);
		rtw88_write32(hw, REG_EFUSE_CTRL, cfg);
		error = rtw88_poll(hw, REG_EFUSE_CTRL, BIT_EF_FLAG, BIT_EF_FLAG, 4, 1, 100000);
		if (error != 0)
			return error;
		physical[at] = rtw88_read8(hw, REG_EFUSE_CTRL);
	}
	error = rtw88_decode_efuse(physical, sizeof(physical) - 96, hw->efuse, sizeof(hw->efuse));
	memset(physical, 0, sizeof(physical));
	if (error != 0)
		return error;
	memcpy(hw->mac, hw->efuse + 0x11a, sizeof(hw->mac));
	if ((hw->mac[0] & 1) != 0 ||
	    memcmp(hw->mac, "\0\0\0\0\0\0", 6) == 0 ||
	    memcmp(hw->mac, "\xff\xff\xff\xff\xff\xff", 6) == 0)
		return ENODEV;
	hw->rfe = hw->efuse[0xca] & 31;
	hw->rfe_btg = hw->rfe == 2 || hw->rfe == 4;
	hw->package = (hw->efuse[0xca] & BIT(5)) != 0 ? 1 : 0;
	if (hw->board == NULL || (hw->board->supported_rfe_mask & (1U << hw->rfe)) == 0 ||
	    (hw->rfe != 0 && hw->rfe != 2 && hw->rfe != 4 && hw->rfe != 6))
		return ENODEV;
	memcpy(hw->calibration, hw->efuse, sizeof(hw->efuse));
	if (hw->rfe == 2 || hw->rfe == 4)
		memcpy(hw->calibration + 0x10, hw->efuse + 0x10 + 42, 18);
	return hw->error;
}

static int
firmware_packet(struct rtw88_hw *hw, const uint8_t *data, size_t len)
{
	struct rtw88_tx_packet tx;
	uint8_t saved_cr, saved_bcn;
	int error;
	if (len > 4096)
		return EMSGSIZE;
	saved_cr = rtw88_read8(hw, REG_CR + 1);
	saved_bcn = rtw88_read8(hw, REG_BCN_CTRL);
	rtw88_write16(hw, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1);
	rtw_write8_set(hw, REG_CR + 1, BIT_ENSWBCN >> 8);
	rtw88_write8(hw, REG_BCN_CTRL, (uint8_t)((saved_bcn & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT));
	memset(&tx, 0, sizeof(tx));
	tx.qsel = 16;
	tx.raw = true;
	error = rtw88_tx_descriptor(hw->scratch, sizeof(hw->scratch), NULL, len, &tx);
	if (error == 0) {
		memcpy(hw->scratch + RTW88_TXDESC, data, len);
		error = rtw88_write_port(hw, tx.qsel, hw->scratch, RTW88_TXDESC + len, false);
	}
	if (error == 0)
		error = rtw88_poll(hw, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1, BIT_BCN_VALID_V1, 4, 10, 10000);
	rtw88_write16(hw, REG_FIFOPAGE_CTRL_2, RTW88_FIFO_BOUNDARY | BIT_BCN_VALID_V1);
	rtw88_write8(hw, REG_BCN_CTRL, saved_bcn);
	rtw88_write8(hw, REG_CR + 1, saved_cr);
	return error != 0 ? error : hw->error;
}

static int
firmware_segment(struct rtw88_hw *hw, const struct rtw88_fw_segment *segment)
{
	size_t at, chunk;
	uint32_t ctrl;
	int error;
	rtw_write32_set(hw, REG_DDMA_CH0CTRL, BIT_DDMACH0_RESET_CHKSUM_STS);
	for (at = 0; at < segment->size; at += chunk) {
		chunk = segment->size - at;
		if (chunk > 4096)
			chunk = 4096;
		error = firmware_packet(hw, segment->data + at, chunk);
		if (error != 0)
			return error;
		error = rtw88_poll(hw, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0, 4, 1, 100000);
		if (error != 0)
			return error;
		ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN | (uint32_t)chunk;
		if (at != 0)
			ctrl |= BIT_DDMACH0_CHKSUM_CONT;
		rtw88_write32(hw, REG_DDMA_CH0SA, OCPBASE_TXBUF_88XX + RTW88_TXDESC);
		rtw88_write32(hw, REG_DDMA_CH0DA, segment->destination + (uint32_t)at);
		rtw88_write32(hw, REG_DDMA_CH0CTRL, ctrl);
		error = rtw88_poll(hw, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0, 4, 1, 100000);
		if (error != 0)
			return error;
	}
	if ((rtw88_read32(hw, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) != 0)
		return EINVAL;
	ctrl = segment->destination < OCPBASE_DMEM_88XX ?
	    BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK : BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK;
	rtw_write8_set(hw, REG_MCUFW_CTRL, ctrl);
	return hw->error;
}

int
rtw88_download_firmware(struct rtw88_hw *hw, const uint8_t *buf, size_t len)
{
	struct rtw88_firmware firmware;
	const uint32_t registers[] = {REG_TXDMA_PQ_MAP + 1, REG_CR, REG_H2CQ_CSR,
	    REG_FIFOPAGE_INFO_1, REG_RQPN_CTRL_2, REG_BCN_CTRL};
	const unsigned int widths[] = {1, 1, 4, 2, 4, 1};
	uint32_t saved[6], val, lte;
	size_t i;
	int error = rtw88_parse_firmware(buf, len, &firmware);
	if (error != 0)
		return error;
	hw->firmware_ready = false;
	error = rtw88_lte_read(hw, LTE_COEX_CTRL, &lte);
	if (error != 0)
		return error;
	for (i = 0; i < 6; i++)
		saved[i] = widths[i] == 1 ? rtw88_read8(hw, registers[i]) : widths[i] == 2 ?
		    rtw88_read16(hw, registers[i]) : rtw88_read32(hw, registers[i]);
	if (hw->error != 0)
		return hw->error;
	saved[2] = BIT_H2CQ_FULL;
	saved[4] |= BIT_LD_RQPN;
	rtw_write8_clr(hw, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
	rtw_write8_clr(hw, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
	rtw88_write8(hw, REG_TXDMA_PQ_MAP + 1, 3 << 6);
	rtw88_write8(hw, REG_CR, BIT_HCI_TXDMA_EN | BIT_TXDMA_EN);
	rtw88_write32(hw, REG_H2CQ_CSR, BIT_H2CQ_FULL);
	rtw88_write16(hw, REG_FIFOPAGE_INFO_1, 0x200);
	rtw88_write32(hw, REG_RQPN_CTRL_2, saved[4]);
	(void)rtw88_read32(hw, RTW88_LOCAL + 0x20);
	rtw88_write8(hw, REG_BCN_CTRL, (uint8_t)((saved[5] & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT));
	rtw_write8_clr(hw, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
	rtw_write8_clr(hw, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
	rtw_write8_set(hw, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
	rtw_write8_set(hw, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
	val = (rtw88_read16(hw, REG_MCUFW_CTRL) & 0x3800) | BIT_MCUFWDL_EN;
	rtw88_write16(hw, REG_MCUFW_CTRL, (uint16_t)val);
	for (i = 0; i < firmware.count; i++) {
		error = firmware_segment(hw, &firmware.segment[i]);
		if (error != 0)
			break;
	}
	for (i = 0; i < 6; i++) {
		if (widths[i] == 1)
			rtw88_write8(hw, registers[i], (uint8_t)saved[i]);
		else if (widths[i] == 2)
			rtw88_write16(hw, registers[i], (uint16_t)saved[i]);
		else
			rtw88_write32(hw, registers[i], saved[i]);
	}
	if (error != 0 || hw->error != 0)
		return error != 0 ? error : hw->error;
	rtw88_write32(hw, REG_TXDMA_STATUS, BTI_PAGE_OVF);
	val = rtw88_read16(hw, REG_MCUFW_CTRL);
	if ((val & BIT_CHECK_SUM_OK) != BIT_CHECK_SUM_OK)
		return EIO;
	rtw88_write16(hw, REG_MCUFW_CTRL, (uint16_t)((val | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN));
	rtw_write8_set(hw, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
	rtw_write8_set(hw, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
	error = rtw88_lte_write(hw, LTE_COEX_CTRL, lte);
	if (error == 0)
		error = rtw88_poll(hw, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY, 4, 10, 100000);
	if (error == 0)
		hw->firmware_ready = true;
	hw->h2c_seq = 0;
	return error;
}

int
rtw88_h2c(struct rtw88_hw *hw, uint8_t id, const uint8_t *data, size_t len)
{
	struct rtw88_tx_packet tx;
	uint8_t packet[H2C_PKT_SIZE];
	int error;
	if (len > H2C_PKT_SIZE - H2C_PKT_HDR_SIZE || !hw->firmware_ready)
		return EINVAL;
	memset(packet, 0, sizeof(packet));
	packet[0] = 1;
	packet[1] = 0xff;
	packet[2] = id;
	rtw88_put16(packet + 4, (uint16_t)(H2C_PKT_HDR_SIZE + len));
	rtw88_put16(packet + 6, hw->h2c_seq++);
	if (len != 0)
		memcpy(packet + H2C_PKT_HDR_SIZE, data, len);
	memset(&tx, 0, sizeof(tx));
	tx.raw = true;
	tx.h2c = true;
	tx.qsel = 19;
	error = rtw88_tx_descriptor(hw->scratch, sizeof(hw->scratch), NULL, sizeof(packet), &tx);
	if (error != 0)
		return error;
	memcpy(hw->scratch + RTW88_TXDESC, packet, sizeof(packet));
	return rtw88_write_port(hw, tx.qsel, hw->scratch, RTW88_TXDESC + sizeof(packet), true);
}

int
rtw88_mac_init(struct rtw88_hw *hw)
{
	uint32_t val;
	uint8_t general[4] = {0, 0, 48, 0};
	uint8_t phydm[8] = {0};
	size_t i;
	int error;
	rtw88_write16(hw, REG_TXDMA_PQ_MAP, (uint16_t)(2 << 4 | 2 << 6 | 1 << 8 | 1 << 10 | 0 << 12 | 3 << 14));
	rtw88_write8(hw, REG_CR, 0);
	rtw88_write8(hw, REG_CR, MAC_TRX_ENABLE);
	rtw88_write32(hw, REG_H2CQ_CSR, BIT_H2CQ_FULL);
	(void)rtw88_read32(hw, RTW88_LOCAL + 0x20);
	rtw88_write32(hw, RTW88_LOCAL, 0);
	rtw88_write16(hw, REG_FIFOPAGE_INFO_1, 16);
	rtw88_write16(hw, REG_FIFOPAGE_INFO_2, 16);
	rtw88_write16(hw, REG_FIFOPAGE_INFO_3, 16);
	rtw88_write16(hw, REG_FIFOPAGE_INFO_4, 14);
	rtw88_write16(hw, REG_FIFOPAGE_INFO_5, 397);
	rtw_write32_set(hw, REG_RQPN_CTRL_2, BIT_LD_RQPN);
	rtw88_write16(hw, REG_FIFOPAGE_CTRL_2, RTW88_FIFO_BOUNDARY);
	rtw_write8_set(hw, REG_FWHW_TXQ_CTRL + 2, BIT_EN_WR_FREE_TAIL >> 16);
	rtw88_write16(hw, REG_BCNQ_BDNY_V1, RTW88_FIFO_BOUNDARY);
	rtw88_write16(hw, REG_FIFOPAGE_CTRL_2 + 2, RTW88_FIFO_BOUNDARY);
	rtw88_write16(hw, REG_BCNQ1_BDNY_V1, RTW88_FIFO_BOUNDARY);
	rtw88_write32(hw, REG_RXFF_BNDY, 16384 - C2H_PKT_BUF - 1);
	rtw_write8_set(hw, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1);
	error = rtw88_poll(hw, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0, 1, 10, 100000);
	if (error != 0)
		return error;
	rtw88_write8(hw, REG_CR + 3, 0);
	val = rtw88_read32(hw, REG_H2C_HEAD);
	rtw88_write32(hw, REG_H2C_HEAD, (val & 0xfffc0000) | RTW88_H2C_ADDRESS);
	val = rtw88_read32(hw, REG_H2C_READ_ADDR);
	rtw88_write32(hw, REG_H2C_READ_ADDR, (val & 0xfffc0000) | RTW88_H2C_ADDRESS);
	val = rtw88_read32(hw, REG_H2C_TAIL);
	rtw88_write32(hw, REG_H2C_TAIL, (val & 0xfffc0000) | (RTW88_H2C_ADDRESS + RTW88_H2C_SIZE));
	rtw88_mask8(hw, REG_H2C_INFO, 3, 1);
	rtw_write8_set(hw, REG_H2C_INFO, 4);
	rtw_write8_set(hw, REG_TXDMA_OFFSET_CHK + 1, 0x80);
	if ((rtw88_read32(hw, REG_H2C_PKT_WRITEADDR) & 0x3ffff) !=
	    (rtw88_read32(hw, REG_H2C_PKT_READADDR) & 0x3ffff))
		return EINVAL;
	error = rtw88_chip_mac_init(hw);
	if (error != 0)
		return error;
	rtw88_write8(hw, REG_RX_DRVINFO_SZ, 4);
	rtw88_mask8(hw, REG_TRXFF_BNDY + 1, 15, 15);
	rtw_write32_set(hw, REG_RCR, BIT_APP_PHYSTS);
	rtw_write32_clr(hw, REG_RCR, BIT_APP_FCS);
	rtw_write32_clr(hw, REG_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9));
	rtw_write8_clr(hw, REG_TXDMA_PQ_MAP, BIT_RXDMA_AGG_EN);
	rtw_write32_clr(hw, REG_RXDMA_AGG_PG_TH, BIT_EN_PRE_CALC);
	for (i = 0; i < sizeof(hw->mac); i++)
		rtw88_write8(hw, 0x0610 + (uint32_t)i, hw->mac[i]);
	phydm[0] = hw->rfe;
	phydm[1] = 4;
	phydm[2] = hw->cut;
	phydm[3] = 0x11;
	error = rtw88_h2c(hw, H2C_PKT_GENERAL_INFO, general, sizeof(general));
	if (error == 0)
		error = rtw88_h2c(hw, H2C_PKT_PHYDM_INFO, phydm, sizeof(phydm));
	return error != 0 ? error : hw->error;
}

static int
rx_fifo(struct rtw88_hw *hw,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg)
{
	size_t total = 0, len, aligned;
	int error;
	while (total < 65536) {
		len = rtw88_read32(hw, REG_SDIO_RX0_REQ_LEN);
		if (hw->error != 0)
			return hw->error;
		if (len == 0)
			break;
		aligned = rtw88_align(hw, len);
		if (len > sizeof(hw->scratch) || aligned == 0 || aligned > sizeof(hw->scratch))
			return EMSGSIZE;
		error = rtw88_read_port(hw, hw->scratch, aligned);
		if (error != 0)
			return error;
		error = rtw88_rx_parse(hw->scratch, len, hw->freq, hw->rfe, input, arg);
		if (error != 0)
			return error;
		total += len;
	}
	return 0;
}

int
rtw88_rx_quiesce(struct rtw88_hw *hw,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg)
{
	unsigned int tries;
	uint8_t control;
	int error;
	if (hw->rx_stopped)
		return hw->error;
	control = rtw88_read8(hw, REG_CR);
	if (hw->error != 0)
		return hw->error;
	hw->rx_was_enabled = (control & BIT_MACRXEN) != 0;
	rtw88_write8(hw, REG_CR, control & ~BIT_MACRXEN);
	if ((rtw88_read8(hw, REG_CR) & BIT_MACRXEN) != 0)
		return hw->error != 0 ? hw->error : EIO;
	hw->rx_stopped = true;
	for (tries = 0; tries < 1000; tries++) {
		error = rx_fifo(hw, input, arg);
		if (error != 0)
			return error;
		if ((rtw88_read32(hw, REG_RXPKT_NUM) & BIT_RXDMA_IDLE) != 0)
			break;
		rtw88_delay(hw, 100);
	}
	error = rx_fifo(hw, input, arg);
	if (error == 0 && tries == 1000) {
		if (hw->failed_register == 0)
			hw->failed_register = REG_RXPKT_NUM;
		error = ETIMEDOUT;
	}
	return error != 0 ? error : hw->error;
}

int
rtw88_rx_resume(struct rtw88_hw *hw)
{
	if (!hw->rx_stopped)
		return hw->error;
	if (hw->rx_was_enabled)
		rtw88_mask8(hw, REG_CR, BIT_MACRXEN, 1);
	if (hw->error == 0)
		hw->rx_stopped = false;
	return hw->error;
}

int
rtw88_tx_quiesce(struct rtw88_hw *hw)
{
	static const uint32_t queues[] = {REG_FIFOPAGE_INFO_1, REG_FIFOPAGE_INFO_2,
	    REG_FIFOPAGE_INFO_3, REG_FIFOPAGE_INFO_4};
	unsigned int at, retry;
	bool empty;
	for (retry = 0; retry < 5; retry++) {
		empty = true;
		for (at = 0; at < sizeof(queues) / sizeof(queues[0]); at++)
			if (rtw88_read16(hw, queues[at]) != rtw88_read16(hw, queues[at] + 2))
				empty = false;
		if (hw->error != 0)
			return hw->error;
		if (empty)
			break;
		rtw88_delay(hw, 20000);
	}
	rtw88_write8(hw, REG_TXPAUSE, 0xff);
	hw->tx_paused = true;
	return hw->error;
}

int
rtw88_key_clear(struct rtw88_hw *hw)
{
	unsigned int word;
	hw->key_valid = false;
	for (word = 0; word < 8; word++) {
		rtw88_write32(hw, RTW_SEC_WRITE_REG, 0);
		rtw88_write32(hw, RTW_SEC_CMD_REG,
		    RTW_SEC_CMD_WRITE_ENABLE | RTW_SEC_CMD_POLLING | (4U << RTW_SEC_CAM_ENTRY_SHIFT) | word);
		if (rtw88_poll(hw, RTW_SEC_CMD_REG, RTW_SEC_CMD_POLLING, 0, 4, 1, 10000) != 0)
			return hw->error != 0 ? hw->error : ETIMEDOUT;
	}
	hw->key_valid = false;
	return hw->error;
}

int
rtw88_key(struct rtw88_hw *hw, const uint8_t addr[6], const uint8_t key[16])
{
	uint32_t content[8];
	int i, error = rtw88_key_clear(hw);
	if (error != 0)
		return error;
	memset(content, 0, sizeof(content));
	content[0] = (4 << 2) | BIT(15) | ((uint32_t)addr[0] << 16) | ((uint32_t)addr[1] << 24);
	content[1] = rtw88_get32(addr + 2);
	for (i = 0; i < 4; i++)
		content[i + 2] = rtw88_get32(key + 4 * i);
	for (i = 7; i >= 0; i--) {
		rtw88_write32(hw, RTW_SEC_WRITE_REG, content[i]);
		rtw88_write32(hw, RTW_SEC_CMD_REG,
		    RTW_SEC_CMD_WRITE_ENABLE | RTW_SEC_CMD_POLLING | (4U << RTW_SEC_CAM_ENTRY_SHIFT) | (unsigned int)i);
		error = rtw88_poll(hw, RTW_SEC_CMD_REG, RTW_SEC_CMD_POLLING, 0, 4, 1, 10000);
		if (error != 0)
			break;
	}
	for (i = 0; i < 8; i++)
		((volatile uint32_t *)content)[i] = 0;
	if (error != 0)
		return error;
	rtw_write32_set(hw, REG_CR, RTW_SEC_ENGINE_EN);
	rtw_write8_set(hw, RTW_SEC_CONFIG, RTW_SEC_TX_DEC_EN | RTW_SEC_RX_DEC_EN);
	hw->key_valid = hw->error == 0;
	return hw->error;
}

int
rtw88_state(struct rtw88_hw *hw, int state, const uint8_t bssid[6],
    uint16_t aid, uint16_t beacon_interval, uint8_t erp, uint16_t response_rates)
{
	size_t i;
	int error;
	if (state < 0 || state > 4)
		return EINVAL;
	if (state != 4) {
		error = rtw88_key_clear(hw);
		if (error != 0)
			return error;
	}
	for (i = 0; i < 6; i++)
		rtw88_write8(hw, 0x0618 + (uint32_t)i, bssid[i]);
	rtw88_mask8(hw, REG_CR + 2, 3, state >= 2 ? 2 : 0);
	rtw88_mask32(hw, 0x06a8, 0x7ff, aid & 0x7ff);
	(void)beacon_interval;
	hw->slot_time = hw->freq > 5000 || (erp & 1) != 0 ? 9 : 20;
	hw->protection = hw->freq < 3000 && (erp & 4) != 0;
	hw->rts_rate = (response_rates & 15) != 0 ? (uint8_t)__builtin_ctz(response_rates & 15) : 0;
	response_rates &= hw->freq > 5000 ? 0xff0 : 0xfff;
	rtw88_mask32(hw, REG_RRSR, 0xfffff, response_rates != 0 ? response_rates :
	    hw->freq > 5000 ? RRSR_INIT_5G : RRSR_INIT_2G);
	rtw_write32_set(hw, REG_RCR, BIT_AM | BIT_AB | BIT_APM);
	if (state == 4)
		rtw_write32_set(hw, REG_RCR, BIT_CBSSID_DATA | BIT_CBSSID_BCN);
	else
		rtw_write32_clr(hw, REG_RCR, BIT_CBSSID_DATA | BIT_CBSSID_BCN);
	rtw88_write8(hw, REG_TXPAUSE, state == 0 ? 0xff : 0);
	hw->tx_paused = state == 0;
	return hw->error;
}

int
rtw88_edca(struct rtw88_hw *hw, unsigned int ac, uint8_t aifsn,
    uint8_t ecwmin, uint8_t ecwmax, uint16_t txop)
{
	static const uint32_t registers[4] = {REG_EDCA_BE_PARAM, REG_EDCA_BK_PARAM, REG_EDCA_VI_PARAM, REG_EDCA_VO_PARAM};
	uint32_t value, aifs;
	if (ac >= 4 || ecwmin > 15 || ecwmax > 15 || aifsn > 15 || ecwmin > ecwmax)
		return EINVAL;
	aifs = (uint32_t)((hw->freq > 5000 ? 16 : 10) +
	    (hw->slot_time == 20 ? 20 : 9) * aifsn);
	if (aifs > 255)
		return EINVAL;
	value = (uint32_t)txop << 16 | (uint32_t)ecwmax << 12 | (uint32_t)ecwmin << 8 | aifs;
	rtw88_write32(hw, registers[ac], value);
	return hw->error;
}

int
rtw88_rx_drain(struct rtw88_hw *hw,
    int (*input)(void *, const struct rtw88_rx_packet *), void *arg)
{
	uint32_t status;
	int error;
	status = rtw88_read32(hw, RTW88_SDIO_HISR);
	if (hw->error != 0)
		return hw->error;
	if ((status & BIT(2)) != 0)
		rtw88_write32(hw, REG_TXDMA_STATUS, rtw88_read32(hw, REG_TXDMA_STATUS));
	error = rx_fifo(hw, input, arg);
	rtw88_write32(hw, RTW88_SDIO_HISR, status & ~BIT(0));
	return error != 0 ? error : hw->error;
}
