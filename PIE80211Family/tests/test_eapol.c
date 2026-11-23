/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include "crypto/aes.h"
#include <crypto/key_wrap.h>
#include <pie80211/family.h>
#include <pie80211/ioctl.h>
#include "host.h"

static struct ieee80211com ic;
static const uint8_t aa[6] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t spa[6] = { 2, 0, 0, 0, 0, 2 };
static const uint8_t rsnie_ccmp[] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
	0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};
static const uint8_t rsnie_tkip_group[] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x01, 0x00, 0x00, 0x0f,
	0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};
static uint8_t pmk[32], anonce[32], gtk[16];
static uint64_t replay;

static struct {
	int count;
	int cipher[4];
	int flags[4];
	int id[4];
	uint8_t key[4][32];
} installed;
static int newstates[8], nnewstates;

static int
test_set_key(struct ieee80211com *c, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	int i = installed.count;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP)
		return EOPNOTSUPP;
	if (i < 4) {
		installed.cipher[i] = k->k_cipher;
		installed.flags[i] = k->k_flags;
		installed.id[i] = k->k_id;
		memcpy(installed.key[i], k->k_key, 32);
		installed.count++;
	}
	return 0;
}

static void
test_delete_key(struct ieee80211com *c, struct ieee80211_node *ni, struct ieee80211_key *k)
{
}

static int
test_newstate(struct ieee80211com *c, enum ieee80211_state nstate, int arg)
{
	if (nnewstates < (int)nitems(newstates))
		newstates[nnewstates++] = nstate;
	c->ic_state = nstate;
	return 0;
}

static void
setup(const uint8_t *rsnie)
{
	struct ieee80211_node *ni;
	int i;

	strlcpy(ic.ic_if.if_xname, "wlan0", sizeof(ic.ic_if.if_xname));
	ic.ic_if.if_softc = &ic;
	ic.ic_opmode = IEEE80211_M_STA;
	ic.ic_caps = IEEE80211_C_RSN;
	ic.ic_channels[1].ic_freq = 2412;
	ic.ic_channels[1].ic_flags = IEEE80211_CHAN_G;
	ic.ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	memcpy(ic.ic_myaddr, spa, 6);
	if_attach(&ic.ic_if);
	ieee80211_ifattach(&ic.ic_if);
	ieee80211_media_init(&ic.ic_if, ieee80211_media_change, ieee80211_media_status);
	ic.ic_set_key = test_set_key;
	ic.ic_delete_key = test_delete_key;
	ic.ic_newstate = test_newstate;

	for (i = 0; i < 32; i++)
		pmk[i] = (uint8_t)(0x10 + i);
	ic.ic_flags |= IEEE80211_F_RSNON | IEEE80211_F_PSK;
	memcpy(ic.ic_psk, pmk, sizeof(pmk));
	ic.ic_rsnprotos = IEEE80211_PROTO_RSN;
	ic.ic_rsnakms = IEEE80211_AKM_PSK;
	ic.ic_rsnciphers = IEEE80211_CIPHER_CCMP;
	ic.ic_rsngroupcipher = IEEE80211_CIPHER_CCMP;
	ic.ic_state = IEEE80211_S_RUN;

	ni = ic.ic_bss;
	memcpy(ni->ni_macaddr, aa, 6);
	memcpy(ni->ni_bssid, aa, 6);
	ni->ni_rsnprotos = IEEE80211_PROTO_RSN;
	ni->ni_rsnakms = IEEE80211_AKM_PSK;
	ni->ni_rsnciphers = ni->ni_rsncipher = IEEE80211_CIPHER_CCMP;
	ni->ni_rsngroupcipher = rsnie[7] == 4 ? IEEE80211_CIPHER_CCMP : IEEE80211_CIPHER_TKIP;
	ieee80211_save_ie(rsnie, &ni->ni_rsnie);
	ni->ni_rsn_supp_state = RSNA_SUPP_PTKSTART;
}

static struct mbuf *
key_frame(uint16_t info, const uint8_t *data, size_t datalen, const uint8_t *kck)
{
	uint8_t buf[512];
	struct ether_header *eh = (struct ether_header *)buf;
	struct ieee80211_eapol_key *key = (struct ieee80211_eapol_key *)(eh + 1);

	memset(buf, 0, sizeof(buf));
	memcpy(eh->ether_dhost, spa, 6);
	memcpy(eh->ether_shost, aa, 6);
	eh->ether_type = htons(ETHERTYPE_EAPOL);
	key->version = 2;
	key->type = EAPOL_KEY;
	key->desc = EAPOL_KEY_DESC_IEEE80211;
	BE_WRITE_2(key->info, info);
	BE_WRITE_2(key->keylen, 16);
	BE_WRITE_8(key->replaycnt, replay);
	memcpy(key->nonce, anonce, sizeof(anonce));
	BE_WRITE_2(key->paylen, datalen);
	BE_WRITE_2(key->len, sizeof(*key) - 4 + datalen);
	if (datalen > 0)
		memcpy(key + 1, data, datalen);
	if (kck != NULL)
		ieee80211_eapol_key_mic(key, kck);
	return pie80211_m_frombuf(buf, sizeof(*eh) + sizeof(*key) + datalen, 0);
}

static struct mbuf *
sent(void)
{
	return pie80211_ifq_dequeue(&ic.ic_if.if_snd);
}

static int
reply(struct ieee80211_eapol_key *key, uint8_t *data, size_t cap)
{
	struct mbuf *m = sent();
	struct ether_header eh;
	int len;

	if (m == NULL)
		return -1;
	m_copydata(m, 0, sizeof(eh), &eh);
	len = m->m_pkthdr.len - (int)sizeof(eh) - (int)sizeof(*key);
	if (ntohs(eh.ether_type) != ETHERTYPE_EAPOL || memcmp(eh.ether_dhost, aa, 6) != 0 ||
	    len < 0 || (size_t)len > cap) {
		m_freem(m);
		return -1;
	}
	m_copydata(m, sizeof(eh), sizeof(*key), key);
	m_copydata(m, sizeof(eh) + sizeof(*key), len, data);
	m_freem(m);
	return len;
}

static int
mic_ok(const struct ieee80211_eapol_key *key, const uint8_t *data, int len, const uint8_t *kck)
{
	uint8_t buf[512];
	struct ieee80211_eapol_key *k = (struct ieee80211_eapol_key *)buf;

	memcpy(buf, key, sizeof(*key));
	memcpy(buf + sizeof(*key), data, (size_t)len);
	return ieee80211_eapol_key_check_mic(k, kck) == 0;
}

static int
handshake(const uint8_t *rsnie, struct ieee80211_ptk *ptk)
{
	struct ieee80211_eapol_key key;
	uint8_t data[256], kd[64], wrapped[72];
	aes_key_wrap_ctx kw;
	size_t kdlen;
	int len;

	replay++;
	ieee80211_eapol_key_input(&ic, key_frame(EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE |
	    EAPOL_KEY_KEYACK, NULL, 0, NULL), ic.ic_bss);
	len = reply(&key, data, sizeof(data));
	CHECK(len > 0);
	if (len <= 0)
		return -1;
	CHECK(BE_READ_2(key.info) == (EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYMIC));
	CHECK(BE_READ_8(key.replaycnt) == replay);
	ieee80211_derive_ptk(IEEE80211_AKM_PSK, pmk, aa, spa, anonce, key.nonce, ptk);
	CHECK(mic_ok(&key, data, len, ptk->kck));
	CHECK(len >= 22 && data[0] == 0x30);

	memcpy(kd, rsnie, 22);
	kd[22] = IEEE80211_ELEMID_VENDOR;
	kd[23] = 22;
	memcpy(&kd[24], IEEE80211_OUI, 3);
	kd[27] = IEEE80211_KDE_GTK;
	kd[28] = 1;
	kd[29] = 0;
	memcpy(&kd[30], gtk, 16);
	kd[46] = IEEE80211_ELEMID_VENDOR;
	kd[47] = 0;
	kdlen = 48;
	aes_key_wrap_set_key(&kw, ptk->kek, 16);
	aes_key_wrap(&kw, kd, kdlen / 8, wrapped);

	replay++;
	ieee80211_eapol_key_input(&ic, key_frame(EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE |
	    EAPOL_KEY_INSTALL | EAPOL_KEY_KEYACK | EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE |
	    EAPOL_KEY_ENCRYPTED, wrapped, kdlen + 8, ptk->kck), ic.ic_bss);
	len = reply(&key, data, sizeof(data));
	CHECK(len == 0);
	if (len != 0)
		return -1;
	CHECK(BE_READ_2(key.info) ==
	    (EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE));
	CHECK(mic_ok(&key, data, 0, ptk->kck));
	return 0;
}

static void
test_wpa2_psk(void)
{
	struct ieee80211_ptk ptk;
	int i;

	for (i = 0; i < 32; i++)
		anonce[i] = (uint8_t)(0x80 + i);
	for (i = 0; i < 16; i++)
		gtk[i] = (uint8_t)(0xc0 + i);
	setup(rsnie_ccmp);
	CHECK(handshake(rsnie_ccmp, &ptk) == 0);
	CHECK(installed.count == 2);
	CHECK(installed.cipher[0] == IEEE80211_CIPHER_CCMP &&
	    memcmp(installed.key[0], ptk.tk, 16) == 0);
	CHECK(installed.id[1] == 1 && memcmp(installed.key[1], gtk, 16) == 0);
	CHECK(ic.ic_bss->ni_port_valid == 1);
	CHECK(ic.ic_bss->ni_rsn_supp_state == RSNA_SUPP_PTKDONE);
	CHECK(sent() == NULL);
}

static void
test_v1_dropped(void)
{
	uint64_t before = ic.ic_stats.is_rx_eapol_key;

	replay++;
	ieee80211_eapol_key_input(&ic, key_frame(EAPOL_KEY_DESC_V1 | EAPOL_KEY_PAIRWISE |
	    EAPOL_KEY_KEYACK, NULL, 0, NULL), ic.ic_bss);
	CHECK(ic.ic_stats.is_rx_eapol_key == before + 1);
	CHECK(sent() == NULL);
}

static void
test_tkip_group_refused(void)
{
	struct ieee80211_ptk ptk;
	struct mbuf *m;
	int before = installed.count;

	ic.ic_bss->ni_rsngroupcipher = IEEE80211_CIPHER_TKIP;
	free(ic.ic_bss->ni_rsnie, M_DEVBUF, 2 + ic.ic_bss->ni_rsnie[1]);
	ic.ic_bss->ni_rsnie = NULL;
	ieee80211_save_ie(rsnie_tkip_group, &ic.ic_bss->ni_rsnie);
	nnewstates = 0;
	CHECK(handshake(rsnie_tkip_group, &ptk) == 0);
	CHECK(installed.count == before + 1);
	CHECK(nnewstates == 1 && newstates[0] == IEEE80211_S_SCAN);
	m = mq_dequeue(&ic.ic_mgtq);
	CHECK(m != NULL);
	m_freem(m);
}

static void
test_status(void)
{
	struct pie80211_status st;
	struct pie80211_com *com = (struct pie80211_com *)&ic;

	CHECK(pie80211_ioctl(com, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.state == PIE80211_STATE_RUN && st.link_up == 1);
	CHECK(memcmp(st.bssid, aa, 6) == 0);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_STATUS, &st, sizeof(st) - 1) == EINVAL);
}

static void
test_join_list(void)
{
	struct pie80211_com *com = (struct pie80211_com *)&ic;
	struct pie80211_join_req req;
	struct pie80211_join_list list;
	struct pie80211_country country = { { 'D', 'E' } };

	memset(&req, 0, sizeof(req));
	req.ssid.len = 4;
	memcpy(req.ssid.bytes, "Home", 4);
	req.security = PIE80211_SEC_WPA2_PSK;
	memset(req.psk, 0x5a, sizeof(req.psk));
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == ENETRESET);
	CHECK(ISSET(ic.ic_flags, IEEE80211_F_AUTO_JOIN));
	req.ssid.len = 4;
	memcpy(req.ssid.bytes, "Cafe", 4);
	req.security = PIE80211_SEC_OPEN;
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == ENETRESET);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_LIST, &list, sizeof(list)) == 0);
	CHECK(list.count == 2 && list.entries[0].security == PIE80211_SEC_WPA2_PSK &&
	    list.entries[1].security == PIE80211_SEC_OPEN);
	memcpy(req.ssid.bytes, "Home", 4);
	req.security = PIE80211_SEC_OPEN;
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == ENETRESET);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_LIST, &list, sizeof(list)) == 0);
	CHECK(list.count == 2 && list.entries[1].security == PIE80211_SEC_OPEN &&
	    memcmp(list.entries[1].ssid.bytes, "Home", 4) == 0);
	req.security = 7;
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == EINVAL);
	req.ssid.len = 33;
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == EINVAL);
	req.ssid.len = 4;
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_DEL, &req, sizeof(req)) == 0);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_DEL, &req, sizeof(req)) == ESRCH);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_LIST, &list, sizeof(list)) == 0 &&
	    list.count == 1);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_FLUSH, NULL, 0) == ENETRESET);
	CHECK(pie80211_ioctl(com, PIE80211_CMD_JOIN_LIST, &list, sizeof(list)) == 0 &&
	    list.count == 0);
	CHECK(!ISSET(ic.ic_flags, IEEE80211_F_AUTO_JOIN));
	CHECK(pie80211_ioctl(com, PIE80211_CMD_COUNTRY, &country, sizeof(country)) == EOPNOTSUPP);
}

int
main(void)
{
	test_wpa2_psk();
	test_status();
	test_v1_dropped();
	test_tkip_group_refused();
	test_join_list();
	ieee80211_ifdetach(&ic.ic_if);
	if_detach(&ic.ic_if);
	return host_report("test_eapol");
}
