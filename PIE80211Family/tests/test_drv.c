/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include "crypto/aes.h"
#include <crypto/key_wrap.h>
#include <pie80211/driver.h>
#include <pie80211/family.h>
#include <pie80211/ioctl.h>
#include "host.h"

static const uint8_t me[6] = { 2, 0, 0, 0, 0, 2 };
static const uint8_t ap[6] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t peer[6] = { 2, 0, 0, 0, 0, 9 };
static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t rsn_ccmp[] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
	0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};
static const uint8_t rsn_tkip_group[] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x01, 0x00, 0x00, 0x0f,
	0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};

static struct {
	struct pie80211_drv *d;
	int started;
	int stopped;
	int nstates;
	struct pie80211_drv_state st;
	int keys;
	int key_deletes;
	struct pie80211_drv_key key;
	struct mbuf_list tx;
	uint32_t txflags[64];
	uint32_t txcookie[64];
	int ntx;
	char country[2];
} fk;
static struct mbuf_list up = MBUF_LIST_INITIALIZER();
static int link_up;
static int tx_wakes;
static int restarts;
static uint16_t seqno;

static void
f_start(void *s)
{
	fk.started++;
	pie80211_drv_started(fk.d, 0);
}

static void
f_stop(void *s)
{
	fk.stopped++;
}

static void
f_set_state(void *s, const struct pie80211_drv_state *st)
{
	fk.st = *st;
	fk.nstates++;
	pie80211_drv_state_done(fk.d);
}

static void
f_set_key(void *s, const struct pie80211_drv_key *k)
{
	fk.keys++;
	fk.key = *k;
}

static void
f_delete_key(void *s, const struct pie80211_drv_key *k)
{
	fk.key_deletes++;
}

static void
f_set_edca(void *s, const struct pie80211_drv_edca e[4])
{
}

static int
f_tx(void *s, struct pie80211_mbuf *m, const struct pie80211_drv_tx *tx)
{
	if (fk.ntx < (int)nitems(fk.txflags)) {
		fk.txflags[fk.ntx] = tx->flags;
		fk.txcookie[fk.ntx] = tx->cookie;
	}
	fk.ntx++;
	ml_enqueue(&fk.tx, m);
	return 0;
}

static int
f_set_country(void *s, const char c[2])
{
	memcpy(fk.country, c, 2);
	return 0;
}

static const struct pie80211_drv_ops ops = {
	f_start, f_stop, f_set_state, f_set_key, f_delete_key, f_set_edca, f_tx, f_set_country
};

static void
h_input(void *ctx, struct pie80211_mbuf *m)
{
	ml_enqueue(&up, m);
}

static void
h_link(void *ctx, int st, uint64_t baud)
{
	link_up = st;
}

static void
h_restart(void *ctx)
{
	restarts++;
	pie80211_drv_down(fk.d);
	pie80211_drv_up(fk.d);
}

static void
h_tx_wake(void *ctx)
{
	tx_wakes++;
}

static void
pump(void)
{
	int i;

	for (i = 0; i < 1000 && pie80211_timeouts_next() <= pie80211_nsecuptime(); i++)
		pie80211_run_pending(NULL);
}

static void
advance(u_int ms)
{
	host_advance((uint64_t)ms * 1000000ULL);
	pump();
}

static void
inject(const uint8_t *b, size_t n, uint32_t flags)
{
	struct pie80211_drv_rx rx = { .flags = flags, .freq = fk.st.freq, .rssi = -40 };

	pie80211_drv_input(fk.d, pie80211_m_frombuf(b, n, 0), &rx);
	pump();
}

static size_t
hdr(uint8_t *b, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3)
{
	memset(b, 0, 24);
	b[0] = fc0;
	b[1] = fc1;
	memcpy(b + 4, a1, 6);
	memcpy(b + 10, a2, 6);
	memcpy(b + 16, a3, 6);
	seqno++;
	b[22] = (uint8_t)(seqno << 4);
	b[23] = (uint8_t)(seqno >> 4);
	return 24;
}

static size_t
rates_ie(uint8_t *b)
{
	static const uint8_t r[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };

	memcpy(b, r, sizeof(r));
	return sizeof(r);
}

static void
beacon(const char *ssid, int chan, const uint8_t *rsn, size_t rsnlen)
{
	uint8_t b[256];
	size_t n = hdr(b, 0x80, 0, bcast, ap, ap);

	memset(b + n, 0, 8);
	n += 8;
	b[n++] = 100;
	b[n++] = 0;
	b[n++] = rsn != NULL ? 0x11 : 0x01;
	b[n++] = 0;
	b[n++] = 0;
	b[n++] = (uint8_t)strlen(ssid);
	memcpy(b + n, ssid, strlen(ssid));
	n += strlen(ssid);
	n += rates_ie(b + n);
	b[n++] = 3;
	b[n++] = 1;
	b[n++] = (uint8_t)chan;
	if (rsn != NULL) {
		memcpy(b + n, rsn, rsnlen);
		n += rsnlen;
	}
	inject(b, n, 0);
}

static struct mbuf *
next_tx(void)
{
	return ml_dequeue(&fk.tx);
}

static int
tx_is(uint8_t fc0, const uint8_t *a1)
{
	struct mbuf *m;
	uint8_t h[24];
	int found = 0;

	while (!found && (m = next_tx()) != NULL) {
		m_copydata(m, 0, sizeof(h), h);
		found = h[0] == fc0 && memcmp(h + 4, a1, 6) == 0;
		m_freem(m);
	}
	return found;
}

static void
drain_tx(void)
{
	ml_purge(&fk.tx);
}

static int
scan_to(const char *ssid, const uint8_t *rsn, size_t rsnlen)
{
	int i, beaconed = 0;

	for (i = 0; i < 100 && fk.st.state != PIE80211_DRV_S_AUTH; i++) {
		if (fk.st.state == PIE80211_DRV_S_SCAN && fk.st.freq == 2437 && !beaconed) {
			beacon(ssid, 6, rsn, rsnlen);
			beaconed = 1;
		}
		advance(201);
	}
	return fk.st.state == PIE80211_DRV_S_AUTH;
}

static void
auth_and_assoc(void)
{
	uint8_t b[128];
	size_t n;

	CHECK(fk.st.freq == 2437 && memcmp(fk.st.bssid, ap, 6) == 0);
	CHECK(tx_is(0xb0, ap));
	n = hdr(b, 0xb0, 0, me, ap, ap);
	memcpy(b + n, "\0\0\2\0\0\0", 6);
	inject(b, n + 6, 0);
	CHECK(fk.st.state == PIE80211_DRV_S_ASSOC);
	CHECK(tx_is(0x00, ap));
	n = hdr(b, 0x10, 0, me, ap, ap);
	memcpy(b + n, "\x01\x00\x00\x00\x01\xc0", 6);
	n += 6;
	n += rates_ie(b + n);
	inject(b, n, 0);
	CHECK(fk.st.state == PIE80211_DRV_S_RUN && fk.st.aid == 1);
}

static struct pie80211_drv *
attach_up(void)
{
	struct pie80211_drv_desc desc;
	struct pie80211_drv_host host = { h_input, h_link, h_restart, h_tx_wake, NULL };
	int error;

	memset(&fk, 0, sizeof(fk));
	memset(&desc, 0, sizeof(desc));
	memcpy(desc.addr, me, 6);
	desc.caps = PIE80211_DRV_CAP_SHORT_PREAMBLE | PIE80211_DRV_CAP_SHORT_SLOT;
	desc.nchan = 3;
	desc.chan[0].freq = 2412;
	desc.chan[1].freq = 2437;
	desc.chan[2].freq = 2462;
	desc.chan[0].flags = desc.chan[1].flags = desc.chan[2].flags = PIE80211_DRV_CHAN_2GHZ;
	fk.d = pie80211_drv_attach(&desc, &ops, &fk, &error);
	CHECK(fk.d != NULL && error == 0);
	pie80211_drv_set_host(fk.d, &host);
	link_up = 0;
	return fk.d;
}

static void
join(const char *ssid, int security, const uint8_t *psk)
{
	struct pie80211_join_req req;

	memset(&req, 0, sizeof(req));
	req.ssid.len = (uint8_t)strlen(ssid);
	memcpy(req.ssid.bytes, ssid, req.ssid.len);
	req.security = (uint8_t)security;
	if (psk != NULL)
		memcpy(req.psk, psk, PIE80211_PSK_LEN);
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == 0);
}

static size_t
data_frame(uint8_t *b, const uint8_t *a1, const uint8_t *a3, uint16_t type, const uint8_t *body,
    size_t len, uint64_t pn)
{
	size_t n = hdr(b, 0x08, pn != 0 ? 0x42 : 0x02, a1, ap, a3);

	if (pn != 0) {
		b[n++] = (uint8_t)pn;
		b[n++] = (uint8_t)(pn >> 8);
		b[n++] = 0;
		b[n++] = 0x20;
		b[n++] = (uint8_t)(pn >> 16);
		b[n++] = (uint8_t)(pn >> 24);
		b[n++] = (uint8_t)(pn >> 32);
		b[n++] = (uint8_t)(pn >> 40);
	}
	memcpy(b + n, "\xaa\xaa\x03\x00\x00\x00", 6);
	n += 6;
	b[n++] = (uint8_t)(type >> 8);
	b[n++] = (uint8_t)type;
	memcpy(b + n, body, len);
	return n + len;
}

static int
delivered(uint16_t type, const uint8_t *src, const uint8_t *body, size_t len)
{
	struct mbuf *m = ml_dequeue(&up);
	struct ether_header eh;
	uint8_t data[256];
	int ok;

	if (m == NULL)
		return 0;
	m_copydata(m, 0, sizeof(eh), &eh);
	m_copydata(m, sizeof(eh), (int)len, data);
	ok = ntohs(eh.ether_type) == type && memcmp(eh.ether_dhost, me, 6) == 0 &&
	    memcmp(eh.ether_shost, src, 6) == 0 && m->m_pkthdr.len == (int)(sizeof(eh) + len) &&
	    memcmp(data, body, len) == 0;
	m_freem(m);
	return ok;
}

static void
test_open(void)
{
	struct pie80211_drv_desc desc;
	struct pie80211_status st;
	uint8_t b[256], h[32], payload[20];
	struct ether_header *eh = (struct ether_header *)b;
	struct mbuf *m;
	size_t len;
	long live;
	int error, i, set, root;

	memset(&desc, 0, sizeof(desc));
	desc.caps = PIE80211_DRV_CAP_MFP;
	desc.nchan = 1;
	desc.chan[0].freq = 2412;
	CHECK(pie80211_drv_attach(&desc, &ops, &fk, &error) == NULL && error == EINVAL);

	attach_up();
	join("Cafe", PIE80211_SEC_OPEN, NULL);
	CHECK(pie80211_drv_up(fk.d) == 0);
	pump();
	CHECK(fk.started == 1 && fk.st.state == PIE80211_DRV_S_SCAN);
	CHECK(scan_to("Cafe", NULL, 0));
	auth_and_assoc();
	CHECK(link_up == 1);
	drain_tx();

	for (i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (uint8_t)(0x40 + i);
	memcpy(eh->ether_dhost, peer, 6);
	memcpy(eh->ether_shost, me, 6);
	eh->ether_type = htons(ETHERTYPE_IP);
	memcpy(b + sizeof(*eh), payload, sizeof(payload));
	CHECK(pie80211_drv_output(fk.d, pie80211_m_frombuf(b, sizeof(*eh) + sizeof(payload), 0)) == 0);
	m = next_tx();
	CHECK(m != NULL && m->m_pkthdr.len == 24 + 8 + (int)sizeof(payload));
	if (m != NULL) {
		m_copydata(m, 0, 32, h);
		CHECK(h[0] == 0x08 && (h[1] & 0x43) == 0x01 && memcmp(h + 4, ap, 6) == 0 &&
		    memcmp(h + 10, me, 6) == 0 && memcmp(h + 16, peer, 6) == 0);
		CHECK(memcmp(h + 24, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8) == 0);
		CHECK((fk.txflags[fk.ntx - 1] & PIE80211_DRV_TX_HW_KEY) == 0);
		m_freem(m);
	}

	inject(b, data_frame(b, me, peer, ETHERTYPE_IP, payload, sizeof(payload), 0), 0);
	CHECK(delivered(ETHERTYPE_IP, peer, payload, sizeof(payload)));
	inject(b, data_frame(b, me, peer, ETHERTYPE_IP, payload, sizeof(payload), 7), PIE80211_DRV_RX_DECRYPTED);
	CHECK(ml_dequeue(&up) == NULL);
	live = host_live();
	inject(b, 9, 0);
	CHECK(ml_dequeue(&up) == NULL && host_live() == live);
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_SCAN, NULL, 0) == EISCONN);

	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.state == PIE80211_STATE_RUN && st.link_up == 1 && st.channel == 6 &&
	    st.ssid.len == 4 && memcmp(st.ssid.bytes, "Cafe", 4) == 0 && st.problem == 0);
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_COUNTRY, "DE", 2) == 0 &&
	    memcmp(fk.country, "DE", 2) == 0);
	CHECK(memcmp(pie80211_drv_addr(fk.d), me, 6) == 0);
	CHECK(pie80211_drv_cmdinfo(PIE80211_CMD_JOIN_ADD, &len, &set, &root) == 0 &&
	    len == sizeof(struct pie80211_join_req) && set == 1 && root == 1);
	CHECK(pie80211_drv_cmdinfo(PIE80211_CMD_SCAN, &len, &set, &root) == 0 && len == 0 && set == 1 &&
	    root == 1);
	CHECK(pie80211_drv_cmdinfo(PIE80211_CMD_STATUS, &len, &set, &root) == 0 &&
	    len == sizeof(struct pie80211_status) && set == 0 && root == 0);
	CHECK(pie80211_drv_cmdinfo(PIE80211_CMD_NODES, &len, &set, &root) == 0 && set == 0 && root == 0);
	CHECK(pie80211_drv_cmdinfo(PIE80211_CMD_JOIN_LIST, &len, &set, &root) == 0 &&
	    len == sizeof(struct pie80211_join_list) && set == 0 && root == 1);
	CHECK(pie80211_drv_cmdinfo(PIE80211_CMD_COUNTRY + 1, &len, &set, &root) == EOPNOTSUPP);
	pie80211_drv_down(fk.d);
	CHECK(fk.stopped == 1 && link_up == 0);
	drain_tx();
	pie80211_drv_detach(fk.d);
}

static void
test_tkip_group(void)
{
	struct pie80211_status st;
	uint8_t psk[32];
	int i, nauth = 0;

	memset(psk, 0x33, sizeof(psk));
	attach_up();
	join("Shop", PIE80211_SEC_WPA2_PSK, psk);
	pie80211_drv_up(fk.d);
	pump();
	for (i = 0; i < 60; i++) {
		if (fk.st.state == PIE80211_DRV_S_SCAN && fk.st.freq == 2437)
			beacon("Shop", 6, rsn_tkip_group, sizeof(rsn_tkip_group));
		if (fk.st.state == PIE80211_DRV_S_AUTH)
			nauth++;
		advance(201);
	}
	CHECK(nauth == 0);
	CHECK(!tx_is(0xb0, ap));
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_GROUP_CIPHER && st.problem_ssid.len == 4 &&
	    memcmp(st.problem_ssid.bytes, "Shop", 4) == 0);
	pie80211_drv_down(fk.d);
	drain_tx();
	pie80211_drv_detach(fk.d);
}

static size_t
eapol_key(uint8_t *b, uint16_t info, uint64_t replay, const uint8_t *nonce, const uint8_t *data,
    size_t datalen, const uint8_t *kck)
{
	struct ieee80211_eapol_key *key = (struct ieee80211_eapol_key *)b;

	memset(key, 0, sizeof(*key));
	key->version = 2;
	key->type = EAPOL_KEY;
	key->desc = EAPOL_KEY_DESC_IEEE80211;
	BE_WRITE_2(key->info, info);
	BE_WRITE_2(key->keylen, 16);
	BE_WRITE_8(key->replaycnt, replay);
	memcpy(key->nonce, nonce, EAPOL_KEY_NONCE_LEN);
	BE_WRITE_2(key->paylen, datalen);
	BE_WRITE_2(key->len, sizeof(*key) - 4 + datalen);
	if (datalen > 0)
		memcpy(key + 1, data, datalen);
	if (kck != NULL)
		ieee80211_eapol_key_mic(key, kck);
	return sizeof(*key) + datalen;
}

static int
eapol_out(struct ieee80211_eapol_key *key, int *protected)
{
	struct mbuf *m = next_tx();
	uint8_t h[32];
	int ok;

	if (m == NULL)
		return 0;
	m_copydata(m, 0, 32, h);
	*protected = (h[1] & 0x40) != 0;
	ok = h[0] == 0x08 && memcmp(h + 24, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8) == 0 &&
	    m->m_pkthdr.len >= 32 + (int)sizeof(*key);
	if (ok)
		m_copydata(m, 32, sizeof(*key), key);
	m_freem(m);
	return ok;
}

static int
hwkey_out(uint64_t pn, size_t paylen)
{
	struct mbuf *m = next_tx();
	uint8_t h[64];
	int hdrlen, ok;

	if (m == NULL)
		return 0;
	hdrlen = ieee80211_get_hdrlen(mtod(m, struct ieee80211_frame *));
	ok = (fk.txflags[fk.ntx - 1] & PIE80211_DRV_TX_HW_KEY) != 0 &&
	    (fk.txflags[fk.ntx - 1] & PIE80211_DRV_TX_STATUS) == 0 &&
	    m->m_pkthdr.len == hdrlen + IEEE80211_CCMP_HDRLEN + 8 + (int)paylen;
	if (ok) {
		m_copydata(m, 0, hdrlen + IEEE80211_CCMP_HDRLEN + 8, h);
		ok = (h[1] & 0x40) != 0 && h[hdrlen] == (uint8_t)pn &&
		    h[hdrlen + 1] == (uint8_t)(pn >> 8) && h[hdrlen + 2] == 0 &&
		    h[hdrlen + 3] == IEEE80211_WEP_EXTIV && h[hdrlen + 4] == (uint8_t)(pn >> 16) &&
		    h[hdrlen + 5] == (uint8_t)(pn >> 24) && h[hdrlen + 6] == 0 && h[hdrlen + 7] == 0 &&
		    memcmp(h + hdrlen + IEEE80211_CCMP_HDRLEN, "\xaa\xaa\x03\x00\x00\x00\x08\x00",
		    8) == 0;
	}
	m_freem(m);
	return ok;
}

static void
test_wpa2(void)
{
	struct ieee80211_eapol_key key;
	struct ieee80211_ptk ptk;
	struct ieee80211_key gk;
	struct ieee80211com *ic;
	aes_key_wrap_ctx kw;
	struct pie80211_status st;
	uint8_t pmk[32], anonce[32], gtk[16], kd[48], wrapped[56], b[512], e[256], payload[20];
	uint32_t cookie;
	size_t n;
	int prot, i;
	struct mbuf *m;

	for (i = 0; i < 32; i++) {
		pmk[i] = (uint8_t)(0x10 + i);
		anonce[i] = (uint8_t)(0x80 + i);
	}
	for (i = 0; i < 16; i++)
		gtk[i] = (uint8_t)(0xc0 + i);
	for (i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (uint8_t)(0x70 + i);
	attach_up();
	join("Home", PIE80211_SEC_WPA2_PSK, pmk);
	pie80211_drv_up(fk.d);
	pump();
	CHECK(scan_to("Home", rsn_ccmp, sizeof(rsn_ccmp)));
	auth_and_assoc();
	CHECK(link_up == 0);
	drain_tx();

	n = eapol_key(e, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYACK, 1, anonce,
	    NULL, 0, NULL);
	inject(b, data_frame(b, me, ap, ETHERTYPE_EAPOL, e, n, 0), 0);
	CHECK(eapol_out(&key, &prot) && !prot);
	CHECK((fk.txflags[fk.ntx - 1] & PIE80211_DRV_TX_STATUS) != 0 && fk.txcookie[fk.ntx - 1] != 0);
	cookie = fk.txcookie[fk.ntx - 1];
	ieee80211_derive_ptk(IEEE80211_AKM_PSK, pmk, ap, me, anonce, key.nonce, &ptk);
	pie80211_drv_tx_done(fk.d, cookie, 1);
	pump();
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
	    st.problem == PIE80211_PROBLEM_NONE);
	pie80211_drv_tx_done(fk.d, cookie, 0);
	pump();
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
	    st.problem == PIE80211_PROBLEM_EAPOL_NOACK && st.problem_ssid.len == 4 &&
	    memcmp(st.problem_ssid.bytes, "Home", 4) == 0);

	pie80211_drv_tx_flow(fk.d, 1);
	CHECK(pie80211_drv_tx_stopped(fk.d) == 1);

	memcpy(kd, rsn_ccmp, sizeof(rsn_ccmp));
	kd[22] = IEEE80211_ELEMID_VENDOR;
	kd[23] = 22;
	memcpy(kd + 24, IEEE80211_OUI, 3);
	kd[27] = IEEE80211_KDE_GTK;
	kd[28] = 1;
	kd[29] = 0;
	memcpy(kd + 30, gtk, 16);
	kd[46] = IEEE80211_ELEMID_VENDOR;
	kd[47] = 0;
	aes_key_wrap_set_key(&kw, ptk.kek, 16);
	aes_key_wrap(&kw, kd, sizeof(kd) / 8, wrapped);
	n = eapol_key(e, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_INSTALL |
	    EAPOL_KEY_KEYACK | EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE | EAPOL_KEY_ENCRYPTED, 2,
	    anonce, wrapped, sizeof(wrapped), ptk.kck);
	inject(b, data_frame(b, me, ap, ETHERTYPE_EAPOL, e, n, 0), 0);
	CHECK(eapol_out(&key, &prot) && !prot);
	CHECK((fk.txflags[fk.ntx - 1] & PIE80211_DRV_TX_STATUS) != 0 &&
	    fk.txcookie[fk.ntx - 1] != 0 && fk.txcookie[fk.ntx - 1] != cookie);
	CHECK(BE_READ_2(key.info) ==
	    (EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE));
	pie80211_drv_tx_flow(fk.d, 1);
	pump();
	CHECK(tx_wakes == 0);
	pie80211_drv_tx_flow(fk.d, 0);
	pump();
	CHECK(tx_wakes == 1 && pie80211_drv_tx_stopped(fk.d) == 0);
	pie80211_drv_tx_flow(fk.d, 0);
	pump();
	CHECK(tx_wakes == 1);
	CHECK(fk.keys == 1 && fk.key.len == 16 && memcmp(fk.key.key, ptk.tk, 16) == 0 &&
	    memcmp(fk.key.addr, ap, 6) == 0);
	CHECK(link_up == 0);
	pie80211_drv_key_done(fk.d, 0);
	pump();
	CHECK(link_up == 1);
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
	    st.problem == PIE80211_PROBLEM_NONE);

	memset(b, 0, sizeof(b));
	memcpy(b, peer, 6);
	memcpy(b + 6, me, 6);
	b[12] = 0x08;
	b[13] = 0x00;
	memcpy(b + 14, payload, sizeof(payload));
	CHECK(pie80211_drv_output(fk.d, pie80211_m_frombuf(b, 14 + sizeof(payload), 0)) == 0);
	CHECK(hwkey_out(1, sizeof(payload)));
	CHECK(pie80211_drv_output(fk.d, pie80211_m_frombuf(b, 14 + sizeof(payload), 64)) == 0);
	CHECK(hwkey_out(2, sizeof(payload)));

	n = data_frame(b, me, peer, ETHERTYPE_IP, payload, sizeof(payload), 1);
	memset(b + n, 0xee, IEEE80211_CCMP_MICLEN);
	inject(b, n + IEEE80211_CCMP_MICLEN, PIE80211_DRV_RX_DECRYPTED);
	CHECK(delivered(ETHERTYPE_IP, peer, payload, sizeof(payload)));
	inject(b, n + IEEE80211_CCMP_MICLEN, PIE80211_DRV_RX_DECRYPTED);
	CHECK(ml_dequeue(&up) == NULL);
	n = data_frame(b, me, peer, ETHERTYPE_IP, payload, sizeof(payload), 2);
	memset(b + n, 0xee, IEEE80211_CCMP_MICLEN);
	inject(b, n + IEEE80211_CCMP_MICLEN, PIE80211_DRV_RX_DECRYPTED);
	CHECK(delivered(ETHERTYPE_IP, peer, payload, sizeof(payload)));

	memset(&gk, 0, sizeof(gk));
	gk.k_cipher = IEEE80211_CIPHER_CCMP;
	gk.k_flags = IEEE80211_KEY_GROUP;
	gk.k_id = 1;
	gk.k_len = 16;
	memcpy(gk.k_key, gtk, 16);
	ic = (struct ieee80211com *)fk.d;
	CHECK(ieee80211_ccmp_set_key(ic, &gk) == 0);
	n = data_frame(b, bcast, peer, ETHERTYPE_IP, payload, sizeof(payload), 0);
	b[1] |= 0x40;
	m = ieee80211_ccmp_encrypt(ic, pie80211_m_frombuf(b, n, 0), &gk);
	CHECK(m != NULL);
	if (m != NULL) {
		n = (size_t)pie80211_m_tobuf(m, b, sizeof(b));
		m_freem(m);
		b[24 + 3] |= 1 << 6;
		inject(b, n, 0);
	}
	ieee80211_ccmp_delete_key(ic, &gk);
	m = ml_dequeue(&up);
	CHECK(m != NULL);
	m_freem(m);

	pie80211_drv_down(fk.d);
	CHECK(fk.key_deletes >= 1 && link_up == 0);
	drain_tx();
	pie80211_drv_detach(fk.d);
}

static void
test_card_reset(void)
{
	struct pie80211_status st;

	attach_up();
	join("Cafe", PIE80211_SEC_OPEN, NULL);
	CHECK(pie80211_drv_up(fk.d) == 0);
	pump();
	CHECK(scan_to("Cafe", NULL, 0));
	auth_and_assoc();
	CHECK(link_up == 1 && fk.started == 1 && fk.stopped == 0);
	drain_tx();

	pie80211_drv_restart(fk.d);
	CHECK(restarts == 0);
	pump();
	CHECK(restarts == 1 && fk.stopped == 1 && fk.started == 2 && link_up == 0);
	CHECK(fk.st.state == PIE80211_DRV_S_SCAN);
	CHECK(next_tx() == NULL);
	CHECK(scan_to("Cafe", NULL, 0));
	auth_and_assoc();
	CHECK(link_up == 1);
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
	    st.state == PIE80211_STATE_RUN && st.link_up == 1);
	pie80211_drv_down(fk.d);
	drain_tx();
	pie80211_drv_detach(fk.d);
}

/*
 * A WPA2 network that drops the station after the handshake's first message: a passphrase problem.
 * When that message's answer was not acknowledged, that problem stays instead. A join asked for after
 * that message is not the network's doing.
 */
static void
test_handshake_ended(void)
{
	struct pie80211_status st;
	uint8_t pmk[32], anonce[32], b[256], e[256];
	size_t n;
	int prot, acked;
	struct ieee80211_eapol_key key;

	memset(pmk, 0x21, sizeof(pmk));
	memset(anonce, 0x42, sizeof(anonce));
	for (acked = 1; acked >= 0; acked--) {
		attach_up();
		join("Home", PIE80211_SEC_WPA2_PSK, pmk);
		pie80211_drv_up(fk.d);
		pump();
		CHECK(scan_to("Home", rsn_ccmp, sizeof(rsn_ccmp)));
		auth_and_assoc();
		drain_tx();
		n = eapol_key(e, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYACK, 1, anonce,
		    NULL, 0, NULL);
		inject(b, data_frame(b, me, ap, ETHERTYPE_EAPOL, e, n, 0), 0);
		CHECK(eapol_out(&key, &prot) && !prot);
		pie80211_drv_tx_done(fk.d, fk.txcookie[fk.ntx - 1], acked);
		pump();
		CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
		    st.problem == (acked ? PIE80211_PROBLEM_NONE : PIE80211_PROBLEM_EAPOL_NOACK));
		n = hdr(b, 0xc0, 0, me, ap, ap);
		b[n++] = 15;
		b[n++] = 0;
		inject(b, n, 0);
		pump();
		CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
		    st.problem == (acked ? PIE80211_PROBLEM_HANDSHAKE : PIE80211_PROBLEM_EAPOL_NOACK) &&
		    st.problem_ssid.len == 4 && memcmp(st.problem_ssid.bytes, "Home", 4) == 0);
		pie80211_drv_down(fk.d);
		drain_tx();
		pie80211_drv_detach(fk.d);
	}

	attach_up();
	join("Home", PIE80211_SEC_WPA2_PSK, pmk);
	pie80211_drv_up(fk.d);
	pump();
	CHECK(scan_to("Home", rsn_ccmp, sizeof(rsn_ccmp)));
	auth_and_assoc();
	drain_tx();
	n = eapol_key(e, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYACK, 1, anonce, NULL, 0, NULL);
	inject(b, data_frame(b, me, ap, ETHERTYPE_EAPOL, e, n, 0), 0);
	CHECK(eapol_out(&key, &prot) && !prot);
	pie80211_drv_tx_done(fk.d, fk.txcookie[fk.ntx - 1], 1);
	join("Away", PIE80211_SEC_WPA2_PSK, pmk);
	pump();
	CHECK(fk.st.state == PIE80211_DRV_S_SCAN);
	CHECK(pie80211_drv_ioctl(fk.d, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 &&
	    st.problem == PIE80211_PROBLEM_NONE);
	pie80211_drv_down(fk.d);
	drain_tx();
	pie80211_drv_detach(fk.d);
}

int
main(void)
{
	test_open();
	test_tkip_group();
	test_wpa2();
	test_handshake_ended();
	test_card_reset();
	ml_purge(&up);
	return host_report("test_drv");
}
