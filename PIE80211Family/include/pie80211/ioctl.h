/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_IOCTL_H
#define _PIE80211_IOCTL_H

#include <stdint.h>

#define PIE80211_CMD_JOIN_ADD		0x50570001
#define PIE80211_CMD_JOIN_DEL		0x50570002
#define PIE80211_CMD_JOIN_FLUSH		0x50570003
#define PIE80211_CMD_JOIN_LIST		0x50570004
#define PIE80211_CMD_SCAN		0x50570005
#define PIE80211_CMD_NODES		0x50570006
#define PIE80211_CMD_STATUS		0x50570007
#define PIE80211_CMD_COUNTRY		0x50570008

#define PIE80211_SSID_MAX		32
#define PIE80211_PSK_LEN		32
#define PIE80211_JOIN_MAX		16
#define PIE80211_NODES_MAX		64

#define PIE80211_SEC_OPEN		0
#define PIE80211_SEC_WPA2_PSK		1
#define PIE80211_SEC_OTHER		2

#define PIE80211_STATE_INIT		0
#define PIE80211_STATE_SCAN		1
#define PIE80211_STATE_AUTH		2
#define PIE80211_STATE_ASSOC		3
#define PIE80211_STATE_RUN		4

#define PIE80211_PROBLEM_NONE		0
#define PIE80211_PROBLEM_GROUP_CIPHER	1
#define PIE80211_PROBLEM_PAIRWISE_CIPHER 2
#define PIE80211_PROBLEM_KEY_INSTALL	3
#define PIE80211_PROBLEM_EAPOL_NOACK	4
#define PIE80211_PROBLEM_HANDSHAKE	5
#define PIE80211_PROBLEM_HANDSHAKE_TIMEOUT	6

struct pie80211_ssid {
	uint8_t len;
	uint8_t bytes[PIE80211_SSID_MAX];
};

struct pie80211_join_req {
	struct pie80211_ssid ssid;
	uint8_t security;
	uint8_t priority;
	uint8_t psk[PIE80211_PSK_LEN];
};

struct pie80211_join_entry {
	struct pie80211_ssid ssid;
	uint8_t security;
	uint8_t priority;
};

struct pie80211_join_list {
	uint32_t count;
	struct pie80211_join_entry entries[PIE80211_JOIN_MAX];
};

struct pie80211_node_info {
	uint8_t bssid[6];
	uint8_t channel;
	uint8_t security;
	int8_t rssi;
	struct pie80211_ssid ssid;
};

struct pie80211_nodes {
	uint32_t count;
	struct pie80211_node_info nodes[PIE80211_NODES_MAX];
};

struct pie80211_status {
	uint32_t state;
	uint32_t link_up;
	uint32_t rate_kbps;
	uint8_t bssid[6];
	uint8_t channel;
	int8_t rssi;
	struct pie80211_ssid ssid;
	uint32_t problem;
	struct pie80211_ssid problem_ssid;
};

struct pie80211_country {
	char code[2];
};

#endif
