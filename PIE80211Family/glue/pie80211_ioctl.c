/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include <net80211/ieee80211_var.h>
#include <pie80211/family.h>
#include <pie80211/ioctl.h>

void
ieee80211_disable_wep(struct ieee80211com *ic)
{
	struct ieee80211_key *key = ic->ic_nw_keys, *end = key + IEEE80211_WEP_NKID;

	for (; key < end; key++) {
		if (key->k_cipher != IEEE80211_CIPHER_NONE)
			ic->ic_delete_key(ic, NULL, key);
		explicit_bzero(key, sizeof(struct ieee80211_key));
	}
	CLR(ic->ic_flags, IEEE80211_F_WEPON);
}

void
ieee80211_disable_rsn(struct ieee80211com *ic)
{
	ic->ic_rsngroupcipher = IEEE80211_CIPHER_NONE;
	ic->ic_rsnciphers = 0;
	ic->ic_rsnakms = 0;
	ic->ic_rsnprotos = 0;
	CLR(ic->ic_flags, IEEE80211_F_RSNON | IEEE80211_F_PSK | IEEE80211_F_MFPR);
	explicit_bzero(ic->ic_psk, sizeof ic->ic_psk);
}

static int
join_add(struct ieee80211com *ic, const struct pie80211_join_req *req)
{
	struct ieee80211_join join;
	struct ieee80211_wpaparams *wpa = &join.i_wpaparams;
	int error;

	if (req->ssid.len == 0 || req->ssid.len > PIE80211_SSID_MAX ||
	    (req->security != PIE80211_SEC_OPEN && req->security != PIE80211_SEC_WPA2_PSK))
		return EINVAL;
	memset(&join, 0, sizeof(join));
	join.i_len = req->ssid.len;
	memcpy(join.i_nwid, req->ssid.bytes, req->ssid.len);
	if (req->security == PIE80211_SEC_WPA2_PSK) {
		join.i_flags = IEEE80211_JOIN_WPA | IEEE80211_JOIN_WPAPSK;
		wpa->i_enabled = 1;
		wpa->i_protos = IEEE80211_WPA_PROTO_WPA2;
		wpa->i_akms = IEEE80211_WPA_AKM_PSK;
		wpa->i_ciphers = IEEE80211_WPA_CIPHER_CCMP;
		wpa->i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
		memcpy(join.i_wpapsk.i_psk, req->psk, sizeof(join.i_wpapsk.i_psk));
	}
	ieee80211_del_ess(ic, (char *)join.i_nwid, join.i_len, 0);
	error = ieee80211_add_ess(ic, &join);
	explicit_bzero(&join, sizeof(join));
	if (error != 0)
		return error;
	SET(ic->ic_flags, IEEE80211_F_AUTO_JOIN);
	return ENETRESET;
}

static int
join_del(struct ieee80211com *ic, const struct pie80211_join_req *req)
{
	int current;

	if (req->ssid.len == 0 || req->ssid.len > PIE80211_SSID_MAX)
		return EINVAL;
	if (ieee80211_get_ess(ic, (const char *)req->ssid.bytes, req->ssid.len) == NULL)
		return ESRCH;
	current = ic->ic_des_esslen == req->ssid.len &&
	    memcmp(ic->ic_des_essid, req->ssid.bytes, req->ssid.len) == 0;
	ieee80211_del_ess(ic, (char *)(uintptr_t)req->ssid.bytes, req->ssid.len, 0);
	if (!current)
		return 0;
	ieee80211_deselect_ess(ic);
	return ENETRESET;
}

static int
join_list(struct ieee80211com *ic, struct pie80211_join_list *out)
{
	struct ieee80211_ess *ess;
	struct pie80211_join_entry *e;

	memset(out, 0, sizeof(*out));
	TAILQ_FOREACH(ess, &ic->ic_ess, ess_next) {
		if (out->count == PIE80211_JOIN_MAX)
			break;
		e = &out->entries[out->count++];
		e->ssid.len = (uint8_t)ess->esslen;
		memcpy(e->ssid.bytes, ess->essid, (size_t)ess->esslen);
		e->security = ISSET(ess->flags, IEEE80211_F_RSNON) ? PIE80211_SEC_WPA2_PSK :
		    PIE80211_SEC_OPEN;
	}
	return 0;
}

static int
node_security(const struct ieee80211_node *ni)
{
	if (ISSET(ni->ni_rsnprotos, IEEE80211_PROTO_RSN) && ISSET(ni->ni_rsnakms, IEEE80211_AKM_PSK) &&
	    ISSET(ni->ni_rsnciphers, IEEE80211_CIPHER_CCMP))
		return PIE80211_SEC_WPA2_PSK;
	if (ISSET(ni->ni_capinfo, IEEE80211_CAPINFO_PRIVACY))
		return PIE80211_SEC_OTHER;
	return PIE80211_SEC_OPEN;
}

static int8_t
node_rssi(const struct ieee80211com *ic, const struct ieee80211_node *ni)
{
	if (ic->ic_max_rssi == 0)
		return (int8_t)ni->ni_rssi;
	return (int8_t)((int)ni->ni_rssi - PIE80211_RSSI_OFFSET);
}

struct nodes_walk {
	struct ieee80211com *ic;
	struct pie80211_nodes *out;
};

static void
nodes_one(void *arg, struct ieee80211_node *ni)
{
	struct nodes_walk *w = arg;
	struct pie80211_node_info *info;

	if (w->out->count == PIE80211_NODES_MAX || ni->ni_esslen > PIE80211_SSID_MAX)
		return;
	info = &w->out->nodes[w->out->count++];
	memcpy(info->bssid, ni->ni_bssid, sizeof(info->bssid));
	info->channel = (uint8_t)ieee80211_chan2ieee(w->ic, ni->ni_chan);
	info->rssi = node_rssi(w->ic, ni);
	info->security = (uint8_t)node_security(ni);
	info->ssid.len = ni->ni_esslen;
	memcpy(info->ssid.bytes, ni->ni_essid, ni->ni_esslen);
}

static int
status(struct ieee80211com *ic, struct pie80211_status *out)
{
	struct ieee80211_node *ni = ic->ic_bss;
	const struct ieee80211_rateset *rs;

	memset(out, 0, sizeof(*out));
	out->state = (uint32_t)ic->ic_state;
	out->link_up = ic->ic_if.if_link_state == LINK_STATE_UP;
	if (ic->ic_state != IEEE80211_S_RUN || ni == NULL)
		return 0;
	memcpy(out->bssid, ni->ni_bssid, sizeof(out->bssid));
	out->channel = (uint8_t)ieee80211_chan2ieee(ic, ni->ni_chan);
	out->rssi = node_rssi(ic, ni);
	if (ni->ni_esslen <= PIE80211_SSID_MAX) {
		out->ssid.len = ni->ni_esslen;
		memcpy(out->ssid.bytes, ni->ni_essid, ni->ni_esslen);
	}
	rs = &ni->ni_rates;
	if (ni->ni_txrate >= 0 && ni->ni_txrate < rs->rs_nrates)
		out->rate_kbps = (rs->rs_rates[ni->ni_txrate] & IEEE80211_RATE_VAL) * 500U;
	return 0;
}

int
pie80211_ioctl(struct pie80211_com *com, uint32_t cmd, void *data, size_t len)
{
	struct ieee80211com *ic = (struct ieee80211com *)com;
	struct nodes_walk walk;

	switch (cmd) {
	case PIE80211_CMD_JOIN_ADD:
		return len == sizeof(struct pie80211_join_req) ? join_add(ic, data) : EINVAL;
	case PIE80211_CMD_JOIN_DEL:
		return len == sizeof(struct pie80211_join_req) ? join_del(ic, data) : EINVAL;
	case PIE80211_CMD_JOIN_FLUSH:
		ieee80211_del_ess(ic, NULL, 0, 1);
		ieee80211_deselect_ess(ic);
		return ENETRESET;
	case PIE80211_CMD_JOIN_LIST:
		return len == sizeof(struct pie80211_join_list) ? join_list(ic, data) : EINVAL;
	case PIE80211_CMD_SCAN:
		if (!ISSET(ic->ic_if.if_flags, IFF_RUNNING))
			return ENETDOWN;
		if (ic->ic_state == IEEE80211_S_RUN)
			return EISCONN;
		if (ic->ic_state != IEEE80211_S_SCAN)
			ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		return 0;
	case PIE80211_CMD_NODES:
		if (len != sizeof(struct pie80211_nodes))
			return EINVAL;
		memset(data, 0, len);
		walk.ic = ic;
		walk.out = data;
		ieee80211_iterate_nodes(ic, nodes_one, &walk);
		return 0;
	case PIE80211_CMD_STATUS:
		return len == sizeof(struct pie80211_status) ? status(ic, data) : EINVAL;
	default:
		return EOPNOTSUPP;
	}
}
