/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * PIEbwfm on the build machine: the sdmmc calls on <pie/sdio.h>, the NVRAM
 * conversion and its MAC address, the firmware allowlist, the control
 * request's wait and its 16-bit numbers, a passive scan reaching
 * net80211's node table and the family's node list, the country, the
 * chip's address, a WPA2-PSK join with its 4-way handshake through the
 * family's ioctls, the networks and keys it refuses, a reset of the card,
 * and the attach and detach that free all they take.
 */
#include "bwfm_pie_priv.h"
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include "crypto/aes.h"
#include <crypto/key_wrap.h>
#include <dev/ic/bwfmreg.h>
#include <dev/ic/bwfmvar.h>
#include <pie/sdio.h>
#include <pie80211/driver.h>
#include <pie80211/family.h>
#include <pie80211/fullmac.h>
#include <pie80211/ioctl.h>
#include <pie80211/platform.h>
#include "../../PIE80211Family/tests/host.h"
#include "fake_bwfm.h"

#define ROOT	((void *)(uintptr_t)0x2001)
#define NODE	((void *)(uintptr_t)0x2002)

extern const struct cfattach bwfm_sdio_ca;
int bwfm_nvram_convert(int, u_char **, size_t *, size_t *);
void bwfm_rx_event(struct bwfm_softc *, struct mbuf *);
int bwfm_fwvar_var_get_int(struct bwfm_softc *, char *, uint32_t *);
int bwfm_fwvar_var_set_int(struct bwfm_softc *, char *, uint32_t);
int bwfm_sdio_txdata(struct bwfm_softc *, struct mbuf *);
void bwfm_init(struct ifnet *);
uint64_t pie80211_timeouts_next(void);
void pie80211_run_pending(void *);

#define SETS	96

static int answer_mode;
static struct {
	uint32_t cmd;
	char name[32];
	uint8_t data[256];
	size_t len;
	int refused;
} sets[SETS];
static int nsets;
static const char *refuse_name;
static int refuse_left;
static struct mbuf_list txq = MBUF_LIST_INITIALIZER();
static uint32_t reqids[8];
static int nreqids;

static const struct {
	uint32_t cmd;
	const char *name;
	uint8_t data[12];
	size_t len;
} canned[] = {
	{ BWFM_C_GET_VERSION, NULL, { BWFM_IO_TYPE_D11AC }, 4 },
	{ BWFM_C_GET_BANDLIST, NULL, { 1, 0, 0, 0, BWFM_BAND_2G }, 12 },
	{ BWFM_C_GET_RSSI, NULL, { 0xcc, 0xff, 0xff, 0xff }, 4 },
	{ BWFM_C_GET_RATE, NULL, { 130 }, 4 },
	{ BWFM_C_GET_VAR, "cur_etheraddr", { 0 }, 6 },
	{ BWFM_C_GET_VAR, "nmode", { 0 }, 4 },
	{ BWFM_C_GET_VAR, "vhtmode", { 0 }, 4 },
	{ BWFM_C_GET_VAR, "scan_ver", { 0 }, 4 },
};

static void
pump(void)
{
	int i;

	for (i = 0; i < 1000 && pie80211_timeouts_next() <= pie80211_nsecuptime(); i++)
		pie80211_run_pending(NULL);
}

static void
test_shim(void)
{
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct sdmmc_function **sf;
	u_char buf[64];

	CHECK(p != NULL && p->node == 2);
	p->sf[0] = (struct sdmmc_function){ 0, FAKE_F1, NULL, p };
	p->sf[1] = (struct sdmmc_function){ 1, FAKE_F1, NULL, p };
	p->sf[2] = (struct sdmmc_function){ 2, FAKE_F1, NULL, p };
	sf = bwfm_pie_sdio_functions(&p->sf[1]);
	CHECK(sf != NULL && sf[2]->pf == FAKE_F2 && sf[2]->number == 2);
	fake_sdio.ncalls = 0;
	(void)sdmmc_io_read_1(sf[0], 0xf1);
	(void)sdmmc_io_read_1(sf[1], 0x1000e);
	(void)sdmmc_io_read_4(sf[1], 0x8000);
	(void)sdmmc_io_read_multi_1(sf[2], 0x8000, buf, sizeof(buf));
	(void)sdmmc_io_write_region_1(sf[1], 0x8000, buf, sizeof(buf));
	(void)sdmmc_io_set_blocklen(sf[2], 512);
	CHECK(fake_sdio.ncalls == 6);
	CHECK(fake_sdio.calls[0].op == FAKE_READ1 && fake_sdio.calls[0].fn == 0 && fake_sdio.calls[0].addr == 0xf1);
	CHECK(fake_sdio.calls[1].op == FAKE_READ1 && fake_sdio.calls[1].fn == 1);
	CHECK(fake_sdio.calls[2].op == FAKE_READ4 && fake_sdio.calls[2].fn == 1);
	CHECK(fake_sdio.calls[3].op == FAKE_READ && fake_sdio.calls[3].fn == 2 && !fake_sdio.calls[3].incr &&
	    fake_sdio.calls[3].len == sizeof(buf));
	CHECK(fake_sdio.calls[4].op == FAKE_WRITE && fake_sdio.calls[4].fn == 1 && fake_sdio.calls[4].incr);
	CHECK(fake_sdio.calls[5].op == FAKE_BLOCK && fake_sdio.calls[5].fn == 2 && fake_sdio.calls[5].len == 512);
	free(sf, M_DEVBUF, 0);
	fake_sdio.no_f2 = true;
	CHECK(bwfm_pie_sdio_functions(&p->sf[1]) == NULL);
	fake_sdio.no_f2 = false;
	free(p, M_DEVBUF, sizeof(*p));
	bwfm_pie_of_clear();
}

static int
nvram(const char *text, size_t *lenp, u_char **out)
{
	size_t n = strlen(text), size = n, len = 0;
	u_char *buf = malloc(n, M_DEVBUF, M_NOWAIT);
	int error;

	memcpy(buf, text, n);
	error = bwfm_nvram_convert(2, &buf, &size, &len);
	*lenp = len;
	*out = buf;
	return error;
}

static int
has(const u_char *buf, size_t len, const char *s)
{
	size_t n = strlen(s) + 1, i;

	for (i = 0; i + n <= len; i++)
		if (memcmp(buf + i, s, n) == 0)
			return 1;
	return 0;
}

static int
macaddr_lines(const u_char *buf, size_t len)
{
	size_t i;
	int n = 0;

	for (i = 0; i + 8 <= len; i++)
		if ((i == 0 || buf[i - 1] == '\0') && memcmp(buf + i, "macaddr=", 8) == 0)
			n++;
	return n;
}

/* The macaddr entry follows a whole entry: one NUL before it, not an empty entry. */
static int
macaddr_after_entry(const u_char *buf, size_t len)
{
	size_t i;

	for (i = 2; i + 8 <= len; i++)
		if (buf[i - 1] == '\0' && memcmp(buf + i, "macaddr=", 8) == 0)
			return buf[i - 2] != '\0';
	return 0;
}

static void
test_nvram(void)
{
	static const char text[] = "# comment\r\nboardflags=0x1\r\n\r\nmacaddr=b8:27:eb:74:f2:6c\nxtalfreq=37400\n";
	static const u_char zero[6], mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const char none[] = "boardflags=0x1\nxtalfreq=37400\n";
	static const char open_end[] = "boardflags=0x1\nxtalfreq=37400";
	static const char bad[] = "macaddr=b8:27:eb:74:f2:zz\n";
	static const char inside[] = "xmacaddr=b8:27:eb:74:f2:6c\n";
	static const u_char shared[6] = { 0xb8, 0x27, 0xeb, 0x74, 0xf2, 0x6c };
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	u_char *buf, got[6];
	uint32_t token;
	size_t len;

	fake_props[0] = (struct fake_prop){ NODE, "local-mac-address", mac, sizeof(mac) };
	CHECK(nvram(text, &len, &buf) == 0);
	CHECK(has(buf, len, "boardflags=0x1") && has(buf, len, "xtalfreq=37400"));
	CHECK(has(buf, len, "macaddr=02:11:22:33:44:55") && macaddr_lines(buf, len) == 1);
	CHECK(macaddr_after_entry(buf, len));
	CHECK(len % 4 == 0 && len >= 8);
	memcpy(&token, buf + len - 4, 4);
	CHECK((token & 0xffff) * 4 == len - 4 && (token >> 16) == (~token & 0xffff));
	free(buf, M_DEVBUF, 0);

	CHECK(nvram(open_end, &len, &buf) == 0);
	CHECK(has(buf, len, "boardflags=0x1") && has(buf, len, "xtalfreq=37400"));
	CHECK(has(buf, len, "macaddr=02:11:22:33:44:55") && macaddr_after_entry(buf, len));
	free(buf, M_DEVBUF, 0);

	fake_props[0].data = zero;
	CHECK(nvram(text, &len, &buf) == 0);
	CHECK(has(buf, len, "macaddr=b8:27:eb:74:f2:6c") && macaddr_lines(buf, len) == 1);
	CHECK(has(buf, len, "boardflags=0x1") && has(buf, len, "xtalfreq=37400"));
	free(buf, M_DEVBUF, 0);

	fake_props[0] = (struct fake_prop){ 0 };
	CHECK(nvram(text, &len, &buf) == 0);
	CHECK(has(buf, len, "macaddr=b8:27:eb:74:f2:6c") && macaddr_lines(buf, len) == 1);
	free(buf, M_DEVBUF, 0);

	CHECK(bwfm_pie_nvram_macaddr((const u_char *)text, sizeof(text) - 1, got) == 1 &&
	    memcmp(got, shared, 6) == 0);
	CHECK(bwfm_pie_nvram_macaddr((const u_char *)none, sizeof(none) - 1, got) == 0);
	CHECK(bwfm_pie_nvram_macaddr((const u_char *)bad, sizeof(bad) - 1, got) == 0);
	CHECK(bwfm_pie_nvram_macaddr((const u_char *)inside, sizeof(inside) - 1, got) == 0);
	free(p, M_DEVBUF, sizeof(*p));
	bwfm_pie_of_clear();
}

static void
test_firmware(void)
{
	static const u_char junk[16] = "not the firmware";
	static const u_char clm[4733];
	u_char *buf;
	size_t len;

	fake_reads = 0;
	CHECK(bwfm_pie_loadfirmware("brcmfmac43455-sdio.raspberrypi,4-model-b.bin", &buf, &len) == ENOENT);
	CHECK(bwfm_pie_loadfirmware("../../etc/master.passwd", &buf, &len) == ENOENT);
	CHECK(bwfm_pie_loadfirmware("brcmfmac43455-sdio.txt", &buf, &len) == ENOENT);
	CHECK(fake_reads == 0);
	CHECK(bwfm_pie_loadfirmware("brcmfmac43455-sdio.bin", &buf, &len) == ENOENT);
	CHECK(fake_reads == 1 && strcmp(fake_read_path[0], "/usr/share/firmware/brcm/brcmfmac43455-sdio.bin") == 0);
	fake_files[0] = (struct fake_file){ "/usr/share/firmware/brcm/brcmfmac43455-sdio.raspberrypi,500.txt",
	    junk, sizeof(junk) };
	CHECK(bwfm_pie_loadfirmware("brcmfmac43455-sdio.raspberrypi,500.txt", &buf, &len) == EINVAL);
	CHECK(buf == NULL && len == 0);
	CHECK(bwfm_pie_firmware_allowed("brcmfmac43455-sdio.clm_blob", clm, sizeof(clm)) == 0);
	CHECK(bwfm_pie_firmware_allowed("brcmfmac43455-sdio.clm_blob", clm, sizeof(clm) - 1) == 0);
	fake_files[0] = (struct fake_file){ 0 };
}

static void
fake_reset(void)
{
	nsets = 0;
	refuse_name = NULL;
	refuse_left = 0;
	nreqids = 0;
}

static int
find_set(const char *name, int from)
{
	int i;

	for (i = from; i >= 0; i--)
		if (sets[i].cmd == BWFM_C_SET_VAR && !sets[i].refused && strcmp(sets[i].name, name) == 0)
			return i;
	return -1;
}

static const uint8_t *
last_set(const char *name, size_t *len)
{
	int i = find_set(name, nsets - 1);

	if (i < 0)
		return NULL;
	*len = sets[i].len;
	return sets[i].data;
}

static int
set_count(const char *name)
{
	int i, n = 0;

	for (i = 0; i < nsets; i++)
		if (sets[i].cmd == BWFM_C_SET_VAR && strcmp(sets[i].name, name) == 0)
			n++;
	return n;
}

static const uint8_t *
last_cmd(uint32_t cmd, size_t *len)
{
	int i;

	for (i = nsets - 1; i >= 0; i--) {
		if (sets[i].cmd == cmd) {
			*len = sets[i].len;
			return sets[i].data;
		}
	}
	return NULL;
}

static int
refused_count(const char *name)
{
	int i, n = 0;

	for (i = 0; i < nsets; i++)
		if (sets[i].cmd == BWFM_C_SET_VAR && sets[i].refused && strcmp(sets[i].name, name) == 0)
			n++;
	return n;
}

static uint32_t
cmd_int(uint32_t cmd)
{
	uint32_t v = 0xdeadbeef;
	int i;

	for (i = nsets - 1; i >= 0; i--) {
		if (sets[i].cmd == cmd && sets[i].len == 4) {
			memcpy(&v, sets[i].data, 4);
			return le32toh(v);
		}
	}
	return v;
}

static int
escan_passive(void)
{
	const struct bwfm_escan_params_v0 *ep;
	const uint8_t *d;
	size_t len = 0;

	d = last_set("escan", &len);
	if (d == NULL || len < sizeof(*ep))
		return 0;
	ep = (const void *)d;
	return ep->scan_params.scan_type == BWFM_SCANTYPE_PASSIVE &&
	    (le32toh(ep->scan_params.channel_num) >> BWFM_CHANNUM_NSSID_SHIFT) == 0;
}

static uint32_t
set_int(const char *name)
{
	const uint8_t *d;
	uint32_t v = 0xdeadbeef;
	size_t len;

	d = last_set(name, &len);
	if (d != NULL && len == 4)
		memcpy(&v, d, 4);
	return le32toh(v);
}

static int
record(struct bwfm_proto_bcdc_dcmd *d)
{
	uint32_t cmd = le32toh(d->hdr.cmd), len = le32toh(d->hdr.len);
	size_t nlen = 0;
	int i, refused;

	if ((le32toh(d->hdr.flags) & BWFM_BCDC_DCMD_SET) == 0 || nsets == SETS)
		return 0;
	i = nsets++;
	memset(&sets[i], 0, sizeof(sets[i]));
	sets[i].cmd = cmd;
	if (cmd == BWFM_C_SET_VAR) {
		nlen = strnlen(d->buf, len);
		if (nlen < len)
			nlen++;
		snprintf(sets[i].name, sizeof(sets[i].name), "%.*s", (int)nlen, d->buf);
	}
	sets[i].len = len - nlen;
	memcpy(sets[i].data, d->buf + nlen, sets[i].len < sizeof(sets[i].data) ? sets[i].len :
	    sizeof(sets[i].data));
	refused = refuse_name != NULL && refuse_left != 0 && strcmp(sets[i].name, refuse_name) == 0;
	if (refused && refuse_left > 0)
		refuse_left--;
	sets[i].refused = refused;
	return refused;
}

static int
fake_txctl(struct bwfm_softc *sc, void *arg)
{
	struct bwfm_proto_bcdc_ctl *ctl = arg;
	struct bwfm_proto_bcdc_dcmd *d;
	char *ans;
	int i, refused;

	bwfm_pie_lock(sc);
	TAILQ_INSERT_TAIL(&sc->sc_bcdc_rxctlq, ctl, next);
	bwfm_pie_unlock(sc);
	d = (void *)ctl->buf;
	if (nreqids < (int)nitems(reqids))
		reqids[nreqids++] = BWFM_BCDC_DCMD_ID_GET(le32toh(d->hdr.flags));
	if (answer_mode == 2 || sc->sc_pie->bus_off)
		return 0;
	refused = record((struct bwfm_proto_bcdc_dcmd *)ctl->buf);
	ans = malloc(ctl->len, M_TEMP, M_NOWAIT);
	memcpy(ans, ctl->buf, ctl->len);
	d = (void *)ans;
	i = le32toh(d->hdr.cmd) == BWFM_C_GET_VAR ? find_set(d->buf, nsets - 1) : -1;
	if (i >= 0) {
		memcpy(d->buf, sets[i].data, sets[i].len < le32toh(d->hdr.len) ? sets[i].len :
		    le32toh(d->hdr.len));
	} else {
		for (i = 0; i < (int)nitems(canned); i++)
			if (canned[i].cmd == le32toh(d->hdr.cmd) && (canned[i].name == NULL ||
			    strcmp(canned[i].name, d->buf) == 0))
				break;
		if (i < (int)nitems(canned) && canned[i].len <= le32toh(d->hdr.len)) {
			memcpy(d->buf, canned[i].data, canned[i].len);
		} else if (le32toh(d->hdr.len) >= 4) {
			memset(d->buf, 0, 4);
			d->buf[0] = 0x2a;
		}
	}
	if (refused) {
		d->hdr.flags = htole32(le32toh(d->hdr.flags) | BWFM_BCDC_DCMD_ERROR);
		d->hdr.status = htole32((uint32_t)-2);
	}
	sc->sc_proto_ops->proto_rxctl(sc, ans, answer_mode == 1 ? ctl->len - 4 : ctl->len);
	free(ans, M_TEMP, ctl->len);
	return 0;
}

static int
fake_txcheck(struct bwfm_softc *sc)
{
	return 0;
}

static int
fake_txdata(struct bwfm_softc *sc, struct mbuf *m)
{
	ml_enqueue(&txq, m);
	return 0;
}

static struct bwfm_bus_ops fake_bus = {
	.bs_txcheck = fake_txcheck,
	.bs_txdata = fake_txdata,
	.bs_txctl = fake_txctl,
};

static struct bwfm_softc *
new_softc(struct bwfm_pie *p)
{
	struct bwfm_softc *sc = malloc(bwfm_sdio_ca.ca_devsize, M_DEVBUF, M_NOWAIT | M_ZERO);

	sc->sc_pie = p;
	p->sc = sc;
	p->os = bwfm_pie_os_alloc(NULL, NULL);
	strlcpy(sc->sc_dev.dv_xname, "bwfm0", sizeof(sc->sc_dev.dv_xname));
	TAILQ_INIT(&sc->sc_bcdc_rxctlq);
	sc->sc_bus_ops = &fake_bus;
	sc->sc_proto_ops = &bwfm_proto_bcdc_ops;
	return sc;
}

static void
free_softc(struct bwfm_pie *p)
{
	bwfm_pie_os_free(p->os);
	free(p->sc, M_DEVBUF, sizeof(*p->sc));
	free(p, M_DEVBUF, sizeof(*p));
	bwfm_pie_of_clear();
}

static void
test_blobs(void)
{
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct bwfm_softc *sc = new_softc(p);
	u_char *ucode, *nvram;
	size_t size, nvsize, nvlen;
	long live = host_live();

	sc->sc_clm = malloc(4733, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_clmsize = 4733;
	sc->sc_txcap = malloc(64, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_txcapsize = 64;
	bwfm_cleanup(sc);
	CHECK(sc->sc_clm == NULL && sc->sc_clmsize == 0 && sc->sc_txcap == NULL && sc->sc_txcapsize == 0);
	CHECK(host_live() == live);
	sc->sc_clm = malloc(4733, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_clmsize = 4733;
	CHECK(bwfm_loadfirmware(sc, "43455", "-sdio", &ucode, &size, &nvram, &nvsize, &nvlen) != 0);
	CHECK(sc->sc_clm == NULL && sc->sc_clmsize == 0 && ucode == NULL && nvram == NULL);
	CHECK(host_live() == live);
	free_softc(p);
}

static void
test_txctl(void)
{
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct bwfm_softc *sc = new_softc(p);
	uint32_t v = 0;

	struct bwfm_proto_bcdc_ctl held;

	fake_reset();
	answer_mode = 0;
	CHECK(bwfm_fwvar_var_get_int(sc, "pie_test", &v) == 0 && v == 0x2a);
	answer_mode = 1;
	CHECK(bwfm_fwvar_var_get_int(sc, "pie_test", &v) != 0);
	answer_mode = 2;
	CHECK(bwfm_fwvar_var_get_int(sc, "pie_test", &v) != 0);
	CHECK(TAILQ_EMPTY(&sc->sc_bcdc_rxctlq));
	answer_mode = 0;

	fake_reset();
	sc->sc_bcdc_reqid = 0xffff;
	CHECK(bwfm_fwvar_var_get_int(sc, "pie_test", &v) == 0);
	CHECK(bwfm_fwvar_var_get_int(sc, "pie_test", &v) == 0);
	CHECK(nreqids == 2 && reqids[0] == 0xffff && reqids[1] == 0);
	memset(&held, 0, sizeof(held));
	held.reqid = 1;
	TAILQ_INSERT_TAIL(&sc->sc_bcdc_rxctlq, &held, next);
	CHECK(bwfm_fwvar_var_get_int(sc, "pie_test", &v) == 0 && nreqids == 3 && reqids[2] == 2);
	TAILQ_REMOVE(&sc->sc_bcdc_rxctlq, &held, next);
	CHECK(TAILQ_EMPTY(&sc->sc_bcdc_rxctlq));
	fake_reset();
	free_softc(p);
}

static struct mbuf *
escan_event(uint32_t status, const char *ssid, const uint8_t bssid[6], int chan, int rssi,
    const uint8_t *rsn, size_t rsnlen)
{
	static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
	struct mbuf *m = m_gethdr(M_DONTWAIT, MT_DATA);
	struct bwfm_event *e;
	struct bwfm_escan_results *res;
	struct bwfm_bss_info *bss;
	uint8_t *ie;
	size_t len, ielen = 0;

	m_clget(m, M_DONTWAIT, MCLBYTES);
	memset(mtod(m, void *), 0, MCLBYTES);
	e = mtod(m, struct bwfm_event *);
	e->msg.event_type = htonl(BWFM_E_ESCAN_RESULT);
	e->msg.status = htonl(status);
	res = (void *)&e[1];
	len = sizeof(*e) + sizeof(*res);
	if (ssid != NULL) {
		bss = &res->bss_info[0];
		ie = (uint8_t *)&bss[1];
		ie[ielen++] = 0;
		ie[ielen++] = (uint8_t)strlen(ssid);
		memcpy(ie + ielen, ssid, strlen(ssid));
		ielen += strlen(ssid);
		ie[ielen++] = 1;
		ie[ielen++] = sizeof(rates);
		memcpy(ie + ielen, rates, sizeof(rates));
		ielen += sizeof(rates);
		ie[ielen++] = 3;
		ie[ielen++] = 1;
		ie[ielen++] = (uint8_t)chan;
		if (rsn != NULL) {
			memcpy(ie + ielen, rsn, rsnlen);
			ielen += rsnlen;
		}
		memcpy(bss->bssid, bssid, 6);
		bss->beacon_period = htole16(100);
		bss->capability = htole16(IEEE80211_CAPINFO_ESS |
		    (rsn != NULL ? IEEE80211_CAPINFO_PRIVACY : 0));
		bss->chanspec = htole16(chan | BWFM_CHANSPEC_D11AC_BW_20 | BWFM_CHANSPEC_D11AC_BND_2G);
		bss->rssi = htole16((uint16_t)rssi);
		bss->ie_offset = htole16(sizeof(*bss));
		bss->ie_length = htole32(ielen);
		bss->length = htole32(sizeof(*bss) + ielen);
		res->bss_count = htole16(1);
		len += sizeof(*bss) + ielen;
	}
	res->buflen = htole32(len - sizeof(*e));
	m->m_len = m->m_pkthdr.len = len;
	return m;
}

static const uint8_t rsn_ccmp[] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
	0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};
static const uint8_t ap[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x06 };
static const uint8_t me[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
static uint8_t pmk[32], anonce[32], gtk[16];
static uint64_t replay;
static struct mbuf_list up = MBUF_LIST_INITIALIZER();
static int link_up, link_calls;

static void
ic_setup(struct bwfm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	int chan;

	sc->sc_io_type = BWFM_IO_TYPE_D11AC;
	bwfm_attach(sc);
	memcpy(ic->ic_myaddr, me, 6);
	CHECK(if_setlladdr(&ic->ic_if, ic->ic_myaddr) == 0);
	CHECK(memcmp(((struct arpcom *)&ic->ic_if)->ac_enaddr, me, 6) == 0);
	for (chan = 1; chan <= 13; chan++) {
		struct ieee80211_channel *c = &ic->ic_channels[chan];

		c->ic_flags = IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN;
		c->ic_freq = ieee80211_ieee2mhz(chan, IEEE80211_CHAN_2GHZ);
	}
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	ieee80211_channel_init(&ic->ic_if);
	sc->sc_initialized = 1;
}

static void
test_scan(void)
{
	static const uint8_t bssid[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x06 };
	static const uint8_t other[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e };
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct bwfm_softc *sc = new_softc(p);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_node *ni;
	struct pie80211_nodes *nodes;
	int i, found = 0;

	fake_reset();
	ic_setup(sc);
	ifp->if_flags |= IFF_UP;
	bwfm_init(ifp);
	pump();
	CHECK(ifp->if_flags & IFF_RUNNING);
	CHECK(ic->ic_state == IEEE80211_S_SCAN);
	CHECK(cmd_int(BWFM_C_SET_PASSIVE_SCAN) == 1 && escan_passive());

	bwfm_rx_event(sc, escan_event(BWFM_E_STATUS_PARTIAL, "PieNet", bssid, 6, -40, NULL, 0));
	pump();
	ni = ieee80211_find_node(ic, bssid);
	CHECK(ni != NULL);
	if (ni != NULL) {
		CHECK(ni->ni_esslen == 6 && memcmp(ni->ni_essid, "PieNet", 6) == 0);
		CHECK(ni->ni_chan == &ic->ic_channels[6]);
	}
	nodes = malloc(sizeof(*nodes), M_DEVBUF, M_NOWAIT | M_ZERO);
	CHECK(pie80211_ioctl((struct pie80211_com *)ic, PIE80211_CMD_NODES, nodes, sizeof(*nodes)) == 0);
	for (i = 0; i < (int)nodes->count && i < PIE80211_NODES_MAX; i++)
		if (memcmp(nodes->nodes[i].bssid, bssid, 6) == 0 && nodes->nodes[i].channel == 6 &&
		    nodes->nodes[i].ssid.len == 6 && memcmp(nodes->nodes[i].ssid.bytes, "PieNet", 6) == 0 &&
		    nodes->nodes[i].rssi == -40)
			found = 1;
	CHECK(found);
	free(nodes, M_DEVBUF, sizeof(*nodes));

	bwfm_rx_event(sc, escan_event(BWFM_E_STATUS_PARTIAL, "Away", other, 14, -70, NULL, 0));
	pump();
	CHECK(ieee80211_find_node(ic, other) == NULL);
	bwfm_rx_event(sc, escan_event(BWFM_E_STATUS_SUCCESS, NULL, NULL, 0, 0, NULL, 0));
	pump();

	ifp->if_flags &= ~(IFF_UP | IFF_RUNNING);
	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	pump();
	(void)bwfm_detach(sc, 0);
	pump();
	free_softc(p);
}

static void
test_country(void)
{
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct bwfm_softc *sc = new_softc(p);
	const uint8_t *c;
	size_t len = 0;

	fake_reset();
	CHECK(bwfm_pie_set_country(sc, "DE") == 0);
	c = last_set("country", &len);
	CHECK(set_count("country") == 1 && c != NULL && len == 12);
	if (c != NULL && len == 12)
		CHECK(memcmp(c, "DE\0\0\xff\xff\xff\xff" "DE\0\0", 12) == 0);

	fake_reset();
	refuse_name = "country";
	refuse_left = 1;
	CHECK(bwfm_pie_set_country(sc, "US") == 0);
	c = last_set("country", &len);
	CHECK(set_count("country") == 2 && c != NULL && len == 12);
	if (c != NULL && len == 12)
		CHECK(memcmp(c, "US\0\0\0\0\0\0" "US\0\0", 12) == 0);

	fake_reset();
	refuse_name = "country";
	refuse_left = -1;
	CHECK(bwfm_pie_set_country(sc, "XX") == EIO);
	CHECK(set_count("country") == 2);
	fake_reset();
	free_softc(p);
}

static void
test_mac(void)
{
	static const uint8_t shared[6] = { 0xb8, 0x27, 0xeb, 0x74, 0xf2, 0x6c };
	static const uint8_t own[6] = { 0xe4, 0x5f, 0x01, 0x6e, 0x10, 0x1b };
	static const uint8_t zero[6];
	static const uint8_t mcast[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
	static const uint8_t *const bad[] = { shared, zero, mcast };
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct bwfm_softc *sc = new_softc(p);
	struct ieee80211com *ic = &sc->sc_ic;
	const uint8_t *d;
	uint8_t first[6];
	size_t len = 0, i;

	fake_reset();
	memcpy(ic->ic_myaddr, own, 6);
	CHECK(bwfm_pie_set_mac(sc, shared) == 0);
	CHECK(set_count("cur_etheraddr") == 0 && memcmp(ic->ic_myaddr, own, 6) == 0);
	CHECK(p->has_mac && memcmp(p->mac, own, 6) == 0);

	for (i = 0; i < nitems(bad); i++) {
		fake_reset();
		p->has_mac = 0;
		memcpy(ic->ic_myaddr, bad[i], 6);
		CHECK(bwfm_pie_set_mac(sc, shared) == 0);
		d = last_set("cur_etheraddr", &len);
		CHECK(d != NULL && len == 6);
		if (d != NULL && len == 6)
			CHECK(memcmp(d, ic->ic_myaddr, 6) == 0 && memcmp(d, shared, 6) != 0 &&
			    (d[0] & 3) == 2 && memcmp(p->mac, d, 6) == 0);
	}

	fake_reset();
	p->has_mac = 0;
	refuse_name = "cur_etheraddr";
	refuse_left = -1;
	memcpy(ic->ic_myaddr, shared, 6);
	CHECK(bwfm_pie_set_mac(sc, shared) == EIO);
	CHECK(memcmp(ic->ic_myaddr, shared, 6) == 0 && !p->has_mac);

	fake_reset();
	memcpy(ic->ic_myaddr, shared, 6);
	CHECK(bwfm_pie_set_mac(sc, shared) == 0);
	memcpy(first, ic->ic_myaddr, 6);
	fake_reset();
	memcpy(ic->ic_myaddr, own, 6);
	CHECK(bwfm_pie_set_mac(sc, shared) == 0);
	d = last_set("cur_etheraddr", &len);
	CHECK(d != NULL && len == 6 && memcmp(d, first, 6) == 0 && memcmp(ic->ic_myaddr, first, 6) == 0);

	fake_reset();
	p->has_mac = 0;
	refuse_name = "cur_etheraddr";
	refuse_left = -1;
	CHECK(bwfm_preinit(sc) != 0);
	CHECK(!p->has_mac);
	fake_reset();
	free_softc(p);
}

static void
h_input(void *ctx, struct pie80211_mbuf *m)
{
	ml_enqueue(&up, m);
}

static void
h_link(void *ctx, int st, uint64_t baud)
{
	link_up = st;
	link_calls++;
}

static void
rx_frame(struct bwfm_softc *sc, const void *frame, size_t len)
{
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m = m_gethdr(M_DONTWAIT, MT_DATA);
	uint8_t *b;

	m_clget(m, M_DONTWAIT, MCLBYTES);
	b = mtod(m, uint8_t *);
	memset(b, 0, 4);
	b[0] = 0x20;
	memcpy(b + 4, frame, len);
	m->m_len = m->m_pkthdr.len = (int)(4 + len);
	sc->sc_proto_ops->proto_rx(sc, m, &ml);
	if_input(&sc->sc_ic.ic_if, &ml);
	pump();
}

static void
rx_event(struct bwfm_softc *sc, uint32_t type, uint32_t status)
{
	struct bwfm_event e;

	memset(&e, 0, sizeof(e));
	memcpy(e.ehdr.ether_dhost, me, 6);
	memcpy(e.ehdr.ether_shost, ap, 6);
	e.ehdr.ether_type = htons(BWFM_ETHERTYPE_LINK_CTL);
	memcpy(e.hdr.oui, BWFM_BRCM_OUI, 3);
	e.hdr.usr_subtype = htons(BWFM_BRCM_SUBTYPE_EVENT);
	e.msg.event_type = htonl(type);
	e.msg.status = htonl(status);
	memcpy(&e.msg.addr, ap, 6);
	rx_frame(sc, &e, sizeof(e));
}

static void
rx_key(struct bwfm_softc *sc, uint16_t info, const uint8_t *data, size_t datalen, const uint8_t *kck)
{
	uint8_t buf[512];
	struct ether_header *eh = (struct ether_header *)buf;
	struct ieee80211_eapol_key *key = (struct ieee80211_eapol_key *)(eh + 1);

	memset(buf, 0, sizeof(buf));
	memcpy(eh->ether_dhost, me, 6);
	memcpy(eh->ether_shost, ap, 6);
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
	rx_frame(sc, buf, sizeof(*eh) + sizeof(*key) + datalen);
}

static int
tx_key(struct ieee80211_eapol_key *key, uint8_t *data, size_t cap)
{
	struct mbuf *m = ml_dequeue(&txq);
	struct ether_header eh;
	int len;

	if (m == NULL)
		return -1;
	m_copydata(m, 0, sizeof(eh), &eh);
	len = m->m_pkthdr.len - (int)sizeof(eh) - (int)sizeof(*key);
	if (ntohs(eh.ether_type) != ETHERTYPE_EAPOL || memcmp(eh.ether_dhost, ap, 6) != 0 ||
	    memcmp(eh.ether_shost, me, 6) != 0 || len < 0 || (size_t)len > cap) {
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

	memcpy(buf, key, sizeof(*key));
	memcpy(buf + sizeof(*key), data, (size_t)len);
	return ieee80211_eapol_key_check_mic((struct ieee80211_eapol_key *)buf, kck) == 0;
}

static const struct bwfm_wsec_key *
wsec_key(int nth)
{
	int i, n = 0;

	for (i = 0; i < nsets; i++)
		if (sets[i].cmd == BWFM_C_SET_VAR && strcmp(sets[i].name, "wsec_key") == 0 &&
		    sets[i].len == sizeof(struct bwfm_wsec_key) && n++ == nth)
			return (const struct bwfm_wsec_key *)(const void *)sets[i].data;
	return NULL;
}

static void
gtk_kde(uint8_t kd[48])
{
	memcpy(kd, rsn_ccmp, sizeof(rsn_ccmp));
	kd[22] = IEEE80211_ELEMID_VENDOR;
	kd[23] = 22;
	memcpy(&kd[24], IEEE80211_OUI, 3);
	kd[27] = IEEE80211_KDE_GTK;
	kd[28] = 1;
	kd[29] = 0;
	memcpy(&kd[30], gtk, 16);
	kd[46] = IEEE80211_ELEMID_VENDOR;
	kd[47] = 0;
}

static const uint8_t rsn_tkip_group[] = {
	0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x01, 0x00, 0x00, 0x0f,
	0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};
static struct pie80211_drv *restart_drv;
static int restarts;
static int bus_resets;
static int bus_reset_error;
static struct bwfm_softc *stop_sc;

static void
h_restart(void *ctx)
{
	restarts++;
	pie80211_drv_down(restart_drv);
	(void)pie80211_drv_up(restart_drv);
}

static int
fake_bus_reset(struct bwfm_softc *sc)
{
	bus_resets++;
	bwfm_cleanup(sc);
	return bus_reset_error;
}

/* An attached bwfm with a country and a WPA2 network, up and scanning. */
static struct bwfm_pie *
join_setup(struct bwfm_softc **scp)
{
	static const struct pie80211_drv_host host = { h_input, h_link, h_restart, NULL, NULL };
	struct pie80211_country country = { { 'D', 'E' } };
	struct pie80211_join_req req;
	struct bwfm_pie *p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	struct bwfm_softc *sc = new_softc(p);
	const uint8_t *d;
	size_t len = 0;
	int i, error;

	for (i = 0; i < 32; i++) {
		pmk[i] = (uint8_t)(0x10 + i);
		anonce[i] = (uint8_t)(0x80 + i);
	}
	for (i = 0; i < 16; i++)
		gtk[i] = (uint8_t)(0xc0 + i);
	fake_reset();
	link_up = link_calls = restarts = 0;
	*scp = sc;
	ic_setup(sc);
	p->drv = restart_drv = bwfm_pie_drv_attach(p, &error);
	CHECK(p->drv != NULL && error == 0);
	if (p->drv == NULL)
		return p;
	pie80211_drv_set_host(p->drv, &host);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_COUNTRY, &country, sizeof(country)) == 0);
	d = last_set("country", &len);
	CHECK(d != NULL && len == 12 && memcmp(d, "DE", 2) == 0);
	memset(&req, 0, sizeof(req));
	req.ssid.len = 6;
	memcpy(req.ssid.bytes, "PieNet", 6);
	req.security = PIE80211_SEC_WPA2_PSK;
	memcpy(req.psk, pmk, sizeof(pmk));
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == 0);
	CHECK(pie80211_drv_up(p->drv) == 0);
	pump();
	CHECK(ISSET(sc->sc_ic.ic_if.if_flags, IFF_RUNNING) && sc->sc_ic.ic_state == IEEE80211_S_SCAN);
	return p;
}

static int
join_scan(struct bwfm_softc *sc, const uint8_t *rsn, size_t rsnlen)
{
	bwfm_rx_event(sc, escan_event(BWFM_E_STATUS_PARTIAL, "PieNet", ap, 6, -40, rsn, rsnlen));
	pump();
	bwfm_rx_event(sc, escan_event(BWFM_E_STATUS_SUCCESS, NULL, NULL, 0, 0, NULL, 0));
	pump();
	return sc->sc_ic.ic_state == IEEE80211_S_AUTH;
}

static int
join_assoc(struct bwfm_softc *sc)
{
	rx_event(sc, BWFM_E_AUTH, BWFM_E_STATUS_SUCCESS);
	if (sc->sc_ic.ic_state != IEEE80211_S_ASSOC)
		return 0;
	rx_event(sc, BWFM_E_ASSOC, BWFM_E_STATUS_SUCCESS);
	rx_event(sc, BWFM_E_LINK, BWFM_E_STATUS_SUCCESS);
	return sc->sc_ic.ic_state == IEEE80211_S_RUN;
}

/* The 4-way handshake as the access point, up to the queued key installs. */
static void
join_4way(struct bwfm_softc *sc, struct ieee80211_ptk *ptk)
{
	struct ieee80211_eapol_key key;
	uint8_t data[256], kd[48], wrapped[56];
	aes_key_wrap_ctx kw;
	int n;

	replay = 1;
	rx_key(sc, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYACK, NULL, 0, NULL);
	n = tx_key(&key, data, sizeof(data));
	CHECK(n >= (int)sizeof(rsn_ccmp) && data[0] == IEEE80211_ELEMID_RSN);
	CHECK(BE_READ_2(key.info) == (EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYMIC));
	CHECK(BE_READ_8(key.replaycnt) == replay);
	ieee80211_derive_ptk(IEEE80211_AKM_PSK, pmk, ap, me, anonce, key.nonce, ptk);
	CHECK(n > 0 && mic_ok(&key, data, n, ptk->kck));

	gtk_kde(kd);
	aes_key_wrap_set_key(&kw, ptk->kek, 16);
	aes_key_wrap(&kw, kd, sizeof(kd) / 8, wrapped);
	replay++;
	rx_key(sc, EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_INSTALL | EAPOL_KEY_KEYACK |
	    EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE | EAPOL_KEY_ENCRYPTED, wrapped, sizeof(wrapped), ptk->kck);
	n = tx_key(&key, data, sizeof(data));
	CHECK(n == 0 && BE_READ_2(key.info) ==
	    (EAPOL_KEY_DESC_V2 | EAPOL_KEY_PAIRWISE | EAPOL_KEY_KEYMIC | EAPOL_KEY_SECURE));
	CHECK(n == 0 && mic_ok(&key, data, 0, ptk->kck));
	pump();
}

static void
join_teardown(struct bwfm_pie *p, struct bwfm_softc *sc)
{
	if (p->drv != NULL)
		pie80211_drv_detach(p->drv);
	p->drv = NULL;
	pump();
	ml_purge(&txq);
	ml_purge(&up);
	(void)bwfm_detach(sc, 0);
	pump();
	fake_reset();
	free_softc(p);
}

static void
test_join(void)
{
	static const uint8_t ip[20] = { 0x45, 0, 0, 20, 0, 0, 0, 0, 64, 17, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2 };
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct pie80211_status st;
	struct ieee80211_ptk ptk;
	const struct bwfm_ext_join_params *jp;
	const struct bwfm_wsec_key *wk;
	const uint8_t *d;
	uint8_t frame[64];
	struct mbuf *m;
	size_t len = 0;

	if (p->drv == NULL)
		goto out;
	CHECK(set_int("sup_wpa") == 0);
	d = last_set("cur_etheraddr", &len);
	CHECK(d != NULL && len == 6 && memcmp(d, me, 6) == 0 && memcmp(ic->ic_myaddr, me, 6) == 0);
	d = last_set("event_msgs", &len);
	CHECK(d != NULL && len > 0 && (d[BWFM_E_SET_SSID / 8] & (1 << (BWFM_E_SET_SSID % 8))) != 0);
	CHECK(cmd_int(BWFM_C_SET_PASSIVE_SCAN) == 1 && escan_passive());

	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)));
	rx_event(sc, BWFM_E_SET_SSID, BWFM_E_STATUS_NO_NETWORKS);
	CHECK(ic->ic_state == IEEE80211_S_SCAN);
	CHECK(ic->ic_des_esslen == 6 && escan_passive());
	rx_event(sc, BWFM_E_SET_SSID, BWFM_E_STATUS_FAIL);
	CHECK(ic->ic_state == IEEE80211_S_SCAN);

	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)));
	CHECK(set_int("wpa_auth") == BWFM_WPA_AUTH_WPA2_PSK && set_int("wsec") == BWFM_WSEC_AES);
	CHECK(set_int("auth") == BWFM_AUTH_OPEN && set_int("mfp") == BWFM_MFP_NONE);
	d = last_set("wpaie", &len);
	CHECK(d != NULL && len == sizeof(rsn_ccmp) && memcmp(d, rsn_ccmp, len) == 0);
	d = last_set("join", &len);
	CHECK(d != NULL && len == sizeof(*jp) + sizeof(uint32_t));
	if (d != NULL && len == sizeof(*jp) + sizeof(uint32_t)) {
		jp = (const void *)d;
		CHECK(le32toh(jp->ssid.len) == 6 && memcmp(jp->ssid.ssid, "PieNet", 6) == 0 &&
		    memcmp(jp->assoc.bssid, ap, 6) == 0);
		CHECK(jp->scan.scan_type == BWFM_SCANTYPE_PASSIVE && le32toh(jp->assoc.chanspec_num) == 1 &&
		    le16toh(jp->assoc.chanspec_list[0]) ==
		    (6 | BWFM_CHANSPEC_D11AC_BW_20 | BWFM_CHANSPEC_D11AC_BND_2G));
	}

	CHECK(join_assoc(sc));
	CHECK(!link_up && ifp->if_link_state != LINK_STATE_UP);
	join_4way(sc, &ptk);

	wk = wsec_key(0);
	CHECK(wk != NULL);
	if (wk != NULL)
		CHECK(le32toh(wk->index) == 0 && le32toh(wk->len) == 16 && memcmp(wk->data, ptk.tk, 16) == 0 &&
		    le32toh(wk->algo) == BWFM_CRYPTO_ALGO_AES_CCM && le32toh(wk->flags) == 0 &&
		    memcmp(wk->ea, ap, 6) == 0);
	wk = wsec_key(1);
	CHECK(wk != NULL);
	if (wk != NULL)
		CHECK(le32toh(wk->index) == 1 && le32toh(wk->len) == 16 && memcmp(wk->data, gtk, 16) == 0 &&
		    le32toh(wk->algo) == BWFM_CRYPTO_ALGO_AES_CCM &&
		    le32toh(wk->flags) == BWFM_WSEC_PRIMARY_KEY);
	CHECK(set_int("wsec") == BWFM_WSEC_AES);
	CHECK(ic->ic_bss->ni_port_valid && link_up && link_calls == 1 && ifp->if_link_state == LINK_STATE_UP);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.state == PIE80211_STATE_RUN && st.link_up && st.channel == 6 &&
	    memcmp(st.bssid, ap, 6) == 0 && st.ssid.len == 6 && memcmp(st.ssid.bytes, "PieNet", 6) == 0 &&
	    st.problem == PIE80211_PROBLEM_NONE);
	CHECK(st.rssi == -52 && st.rate_kbps == 65000);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_SCAN, NULL, 0) == EISCONN);
	CHECK(ic->ic_state == IEEE80211_S_RUN);

	memcpy(frame, me, 6);
	memcpy(frame + 6, ap, 6);
	frame[12] = 0x08;
	frame[13] = 0x00;
	memcpy(frame + 14, ip, sizeof(ip));
	rx_frame(sc, frame, 14 + sizeof(ip));
	m = ml_dequeue(&up);
	CHECK(m != NULL && m->m_pkthdr.len == 14 + (int)sizeof(ip));
	if (m != NULL)
		m_freem(m);
	memcpy(frame, ap, 6);
	memcpy(frame + 6, me, 6);
	CHECK(pie80211_drv_output(p->drv, pie80211_m_frombuf(frame, 14 + sizeof(ip), 0)) == 0);
	m = ml_dequeue(&txq);
	CHECK(m != NULL && m->m_pkthdr.len == 14 + (int)sizeof(ip));
	if (m != NULL)
		m_freem(m);
	CHECK(ml_empty(&txq) && ml_empty(&up));
out:
	join_teardown(p, sc);
}

/*
 * A WPA2 network that drops the station before the keys: a passphrase
 * problem, cleared at the next link. A join asked for during the
 * handshake is not one.
 */
static void
test_handshake_ended(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct pie80211_status st;
	struct pie80211_join_req req;
	struct ieee80211_ptk ptk;

	if (p->drv == NULL)
		goto out;
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	CHECK(!ic->ic_bss->ni_port_valid);
	rx_event(sc, BWFM_E_DEAUTH, BWFM_E_STATUS_SUCCESS);
	CHECK(ic->ic_state == IEEE80211_S_SCAN);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_HANDSHAKE && st.problem_ssid.len == 6 &&
	    memcmp(st.problem_ssid.bytes, "PieNet", 6) == 0);
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	CHECK(link_up && ic->ic_bss->ni_port_valid);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_NONE);
	rx_event(sc, BWFM_E_DEAUTH, BWFM_E_STATUS_SUCCESS);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_NONE);
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	CHECK(!ic->ic_bss->ni_port_valid);
	memset(&req, 0, sizeof(req));
	req.ssid.len = 7;
	memcpy(req.ssid.bytes, "PieNet2", 7);
	req.security = PIE80211_SEC_WPA2_PSK;
	memcpy(req.psk, pmk, sizeof(pmk));
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_JOIN_ADD, &req, sizeof(req)) == 0);
	pump();
	CHECK(ic->ic_state == IEEE80211_S_SCAN);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_NONE);
out:
	join_teardown(p, sc);
}

/*
 * The firmware's signal and rate, asked once a second at most, and after a
 * failure once in ten. A rejoin within the second asks again.
 */
static void
test_link_cache(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	struct pie80211_status st;
	struct ieee80211_ptk ptk;
	int asked;

	if (p->drv == NULL)
		goto out;
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	nreqids = 0;
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.rssi == -52 && st.rate_kbps == 65000);
	asked = nreqids;
	CHECK(asked == 2);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.rssi == -52 && st.rate_kbps == 65000 && nreqids == asked);
	host_advance(1100000000ULL);
	answer_mode = 2;
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	answer_mode = 0;
	CHECK(nreqids == asked + 1);
	asked = nreqids;
	host_advance(2000000000ULL);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 && nreqids == asked);
	host_advance(9000000000ULL);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(nreqids == asked + 2 && st.rssi == -52 && st.rate_kbps == 65000);
	rx_event(sc, BWFM_E_DEAUTH, BWFM_E_STATUS_SUCCESS);
	CHECK(sc->sc_ic.ic_state == IEEE80211_S_SCAN);
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	nreqids = 0;
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0 && nreqids == 2);
out:
	join_teardown(p, sc);
}

static void
test_join_fallback(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	const struct bwfm_join_params *jp;
	const uint8_t *d;
	size_t len = 0;

	if (p->drv == NULL)
		goto out;
	refuse_name = "join";
	refuse_left = -1;
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && refused_count("join") == 1);
	d = last_cmd(BWFM_C_SET_SSID, &len);
	CHECK(d != NULL && len == sizeof(*jp) + sizeof(uint32_t));
	if (d != NULL && len == sizeof(*jp) + sizeof(uint32_t)) {
		jp = (const void *)d;
		CHECK(le32toh(jp->ssid.len) == 6 && memcmp(jp->ssid.ssid, "PieNet", 6) == 0 &&
		    memcmp(jp->assoc.bssid, ap, 6) == 0 && le32toh(jp->assoc.chanspec_num) == 1 &&
		    le16toh(jp->assoc.chanspec_list[0]) ==
		    (6 | BWFM_CHANSPEC_D11AC_BW_20 | BWFM_CHANSPEC_D11AC_BND_2G));
	}
	refuse_name = NULL;
	CHECK(join_assoc(sc));
out:
	join_teardown(p, sc);
}

static void
test_refusals(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct pie80211_status st;
	struct ieee80211_key k;
	int i;

	if (p->drv == NULL)
		goto out;
	for (i = 0; i < 3; i++) {
		CHECK(!join_scan(sc, rsn_tkip_group, sizeof(rsn_tkip_group)));
		CHECK(ic->ic_state == IEEE80211_S_SCAN);
	}
	CHECK(set_count("join") == 0 && escan_passive());
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_GROUP_CIPHER && st.problem_ssid.len == 6 &&
	    memcmp(st.problem_ssid.bytes, "PieNet", 6) == 0);

	memset(&k, 0, sizeof(k));
	k.k_cipher = IEEE80211_CIPHER_TKIP;
	k.k_flags = IEEE80211_KEY_GROUP;
	k.k_id = 1;
	k.k_len = 32;
	CHECK(ic->ic_set_key(ic, ic->ic_bss, &k) == EOPNOTSUPP);
	pump();
	CHECK(wsec_key(0) == NULL);
out:
	join_teardown(p, sc);
}

static void
test_key_refused(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct pie80211_status st;
	struct ieee80211_ptk ptk;

	if (p->drv == NULL)
		goto out;
	refuse_name = "wsec_key";
	refuse_left = -1;
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	CHECK(!link_up && !ic->ic_bss->ni_port_valid && ic->ic_state == IEEE80211_S_SCAN);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_KEY_INSTALL && !st.link_up);
	refuse_name = NULL;
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	CHECK(link_up && ic->ic_bss->ni_port_valid);
	CHECK(pie80211_drv_ioctl(p->drv, PIE80211_CMD_STATUS, &st, sizeof(st)) == 0);
	CHECK(st.problem == PIE80211_PROBLEM_NONE && st.link_up);
out:
	join_teardown(p, sc);
}

static void
test_reset(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p = join_setup(&sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_ptk ptk;
	const uint8_t *d;
	size_t len = 0;
	int sleeps;

	if (p->drv == NULL)
		goto out;
	p->bus_reset = fake_bus_reset;
	bus_resets = bus_reset_error = 0;
	memcpy(p->mac, me, 6);
	p->has_mac = 1;
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	CHECK(link_up && ic->ic_bss->ni_port_valid);

	fake_reset();
	sc->sc_io_type = 0;
	p->generation = p->reset_seen = 4;
	fake_sdio.generation = 5;
	bwfm_pie_card_reset(p);
	CHECK(p->bus_off && p->reload && restarts == 0);
	sleeps = fake_sleeps;
	CHECK(bwfm_fwvar_var_set_int(sc, "pie_test", 1) != 0 && set_count("pie_test") == 0);
	CHECK(fake_sleeps == sleeps);
	pump();
	CHECK(restarts == 1 && bus_resets == 1 && !p->bus_off && !p->reload && p->reload_fails == 0);
	CHECK(p->generation == 5);
	CHECK(sc->sc_io_type == BWFM_IO_TYPE_D11AC && set_count("cur_etheraddr") == 2);
	CHECK(!link_up && (ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING));
	CHECK(ic->ic_state == IEEE80211_S_SCAN && escan_passive() && sc->sc_initialized);
	d = last_set("cur_etheraddr", &len);
	CHECK(d != NULL && len == 6 && memcmp(d, me, 6) == 0 && memcmp(ic->ic_myaddr, me, 6) == 0);
	d = last_set("country", &len);
	CHECK(d != NULL && len == 12 && memcmp(d, "DE", 2) == 0);
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	CHECK(link_up && ic->ic_bss->ni_port_valid);

	refuse_name = "country";
	refuse_left = -1;
	fake_sdio.generation++;
	bwfm_pie_card_reset(p);
	pump();
	CHECK(restarts == 2 && bus_resets == 2 && refused_count("country") == 2);
	CHECK(p->bus_off && p->reload && p->reload_fails == 1);
	CHECK((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == 0 && !link_up);
	refuse_name = NULL;
	CHECK(pie80211_drv_up(p->drv) == 0);
	CHECK(bus_resets == 3 && (ifp->if_flags & IFF_RUNNING) && !p->bus_off && !p->reload &&
	    p->reload_fails == 0);
	d = last_set("country", &len);
	CHECK(d != NULL && len == 12 && memcmp(d, "DE", 2) == 0);

	bus_reset_error = EIO;
	fake_sdio.generation++;
	bwfm_pie_card_reset(p);
	pump();
	CHECK(restarts == 3 && bus_resets == 4 && p->bus_off && p->reload && p->reload_fails == 1);
	CHECK((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == 0 && !link_up);
	p->reload_fails = 3;
	fake_sdio.generation++;
	bwfm_pie_card_reset(p);
	pump();
	CHECK(restarts == 3 && bus_resets == 4);
	bus_reset_error = 0;
	CHECK(pie80211_drv_up(p->drv) == 0);
	CHECK((ifp->if_flags & IFF_RUNNING) && !p->bus_off && !p->reload && p->reload_fails == 0);
out:
	join_teardown(p, sc);
}

static void
stop_frame(void)
{
	struct mbuf *m = m_gethdr(M_DONTWAIT, MT_DATA);
	uint8_t *b;

	m_clget(m, M_DONTWAIT, MCLBYTES);
	b = mtod(m, uint8_t *);
	memset(b, 0, 64);
	b[0] = 0x20;
	memcpy(b + 4, me, 6);
	memcpy(b + 10, ap, 6);
	b[16] = 0x08;
	m->m_len = m->m_pkthdr.len = 64;
	bwfm_pie_rx_defer(stop_sc, m);
}

static void
run_tasks(void)
{
	pump();
}

static void
test_detach(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p;
	struct mbuf *m;
	long live = host_live();

	p = join_setup(&sc);
	p->attached = 1;
	p->reset_on = 1;
	stop_sc = sc;
	stop_frame();
	fake_log[0] = '\0';
	fake_kicks_stopped = 0;
	fake_stop_hook = stop_frame;
	host_gate_hook = run_tasks;
	bwfm_pie_detach(p);
	host_gate_hook = NULL;
	fake_stop_hook = NULL;
	CHECK(strcmp(fake_log, "reset-off stop free") == 0);
	CHECK(p->sc == NULL && p->drv == NULL && p->os == NULL && fake_kicks_stopped == 0);
	CHECK(fake_sdio.reset_handler == NULL);
	bwfm_pie_delete(p);
	ml_purge(&up);
	ml_purge(&txq);
	fake_reset();
	CHECK(host_live() == live);

	p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	sc = new_softc(p);
	ic_setup(sc);
	bwfm_rx_event(sc, escan_event(BWFM_E_STATUS_PARTIAL, "PieNet", ap, 6, -40, NULL, 0));
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	m->m_len = m->m_pkthdr.len = 0;
	CHECK(bwfm_sdio_txdata(sc, m) == 0);
	CHECK(!ml_empty(&sc->sc_evml));
	(void)bwfm_sdio_ca.ca_detach(&sc->sc_dev, 0);
	CHECK(ml_empty(&sc->sc_evml));
	pump();
	free_softc(p);
	CHECK(host_live() == live);
}

/* Refused wsec_key sets that carried a key, as an install does and a delete does not. */
static int
refused_installs(void)
{
	const struct bwfm_wsec_key *k;
	int i, n = 0;

	for (i = 0; i < nsets; i++) {
		k = (const void *)sets[i].data;
		if (sets[i].cmd == BWFM_C_SET_VAR && sets[i].refused && strcmp(sets[i].name, "wsec_key") == 0 &&
		    sets[i].len == sizeof(*k) && le32toh(k->len) != 0)
			n++;
	}
	return n;
}

static void
test_detach_key(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p;
	struct ieee80211com *ic;
	struct ieee80211_ptk ptk;
	long live = host_live();

	p = join_setup(&sc);
	ic = &sc->sc_ic;
	if (p->drv == NULL) {
		join_teardown(p, sc);
		return;
	}
	CHECK(join_scan(sc, rsn_ccmp, sizeof(rsn_ccmp)) && join_assoc(sc));
	join_4way(sc, &ptk);
	CHECK(link_up && ic->ic_bss->ni_pairwise_key.k_cipher == IEEE80211_CIPHER_CCMP);
	refuse_name = "wsec_key";
	refuse_left = -1;
	CHECK(ic->ic_set_key(ic, ic->ic_bss, &ic->ic_bss->ni_pairwise_key) == EBUSY);
	CHECK(refused_installs() == 0);
	p->attached = 1;
	host_gate_exit_hook = run_tasks;
	bwfm_pie_detach(p);
	host_gate_exit_hook = NULL;
	CHECK(refused_installs() == 1);
	CHECK(p->sc == NULL && p->drv == NULL && p->os == NULL);
	bwfm_pie_delete(p);
	ml_purge(&up);
	ml_purge(&txq);
	fake_reset();
	CHECK(host_live() == live);
}

/*
 * A new set-up of the card after the attach's and before the reset
 * callback is there: seen by its count once the callback is set, and
 * handled as the callback would, with a restart. One that the callback
 * sees as well is handled once. With no new set-up, nothing.
 */
static void
test_reset_window(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p;
	long live = host_live();
	int c;

	for (c = 0; c < 3; c++) {
		p = join_setup(&sc);
		p->bus_reset = fake_bus_reset;
		bus_resets = bus_reset_error = 0;
		p->generation = p->reset_seen = 3;
		fake_sdio.generation = c == 0 ? 3 : 4;
		fake_sdio.reset_at_register = c == 2;
		fake_sdio.claims = fake_sdio.releases = 0;
		bwfm_pie_reset_watch(p);
		fake_sdio.reset_at_register = 0;
		CHECK(p->reset_on && fake_sdio.claims == 1 && fake_sdio.releases == 1);
		CHECK(p->resets == (c == 0 ? 0u : 1u) && p->bus_off == (c != 0) && p->reload == (c != 0));
		pump();
		CHECK(restarts == (c == 0 ? 0 : 1) && bus_resets == (c == 0 ? 0 : 1) && !p->bus_off);
		join_teardown(p, sc);
		fake_sdio.reset_handler = NULL;
		fake_sdio.reset_arg = NULL;
	}
	fake_sdio.generation = 0;
	CHECK(host_live() == live);
}

/*
 * The counts only go up. A callback that reads a newer count than the one
 * the attach's check saved handles the set-up, and the check's stale
 * count, and a later callback for the same count, are passed over. 0, the
 * card given up, is handled once, and nothing after it. Counts compare
 * across a wrap.
 */
static void
test_reset_order(void)
{
	struct bwfm_softc *sc;
	struct bwfm_pie *p;
	long live = host_live();

	p = join_setup(&sc);
	p->bus_reset = fake_bus_reset;
	bus_resets = bus_reset_error = 0;
	p->generation = p->reset_seen = 3;
	fake_sdio.generation = 4;
	fake_sdio.reset_at_release = 5;
	bwfm_pie_reset_watch(p);
	CHECK(fake_sdio.reset_at_release == 0 && p->resets == 1 && p->reset_seen == 5);
	bwfm_pie_card_reset(p);
	CHECK(p->resets == 1 && p->reset_seen == 5);
	pump();
	CHECK(restarts == 1 && bus_resets == 1 && !p->bus_off && !p->reload);
	fake_sdio.generation = 0;
	bwfm_pie_card_reset(p);
	CHECK(p->resets == 2 && p->reset_seen == 0 && p->bus_off && p->reload);
	bwfm_pie_card_reset(p);
	fake_sdio.generation = 6;
	bwfm_pie_card_reset(p);
	CHECK(p->resets == 2 && p->reset_seen == 0);
	pump();
	p->reset_seen = 0xfffffffeU;
	fake_sdio.generation = 0xffffffffU;
	bwfm_pie_card_reset(p);
	CHECK(p->resets == 3 && p->reset_seen == 0xffffffffU);
	fake_sdio.generation = 1;
	bwfm_pie_card_reset(p);
	CHECK(p->resets == 4 && p->reset_seen == 1);
	fake_sdio.generation = 0xfffffffeU;
	bwfm_pie_card_reset(p);
	CHECK(p->resets == 4 && p->reset_seen == 1);
	pump();
	join_teardown(p, sc);
	fake_sdio.reset_handler = NULL;
	fake_sdio.reset_arg = NULL;
	fake_sdio.generation = 0;
	CHECK(host_live() == live);
}

static int passive_claimed;

static void
passive_word(const char *w)
{
	if (fake_sdio.claims - fake_sdio.releases != 1)
		passive_claimed = 0;
	fake_logw(w);
}

static void
passive_clocks(struct bwfm_softc *sc, int on)
{
	passive_word(on ? "clocks-on" : "clocks-off");
}

static void
passive_core_reset(struct bwfm_softc *sc, struct bwfm_core *core, uint32_t prereset, uint32_t reset,
    uint32_t postreset)
{
	passive_word(core->co_id == BWFM_AGENT_CORE_ARM_CR4 && (reset & BWFM_AGENT_IOCTL_ARMCR4_CPUHALT) != 0 ?
	    "cr4-halt" : "reset-other");
}

static void
passive_core_disable(struct bwfm_softc *sc, struct bwfm_core *core, uint32_t prereset, uint32_t reset)
{
	passive_word(core->co_id == BWFM_AGENT_CORE_80211 ? "d11-off" : "disable-core");
}

static uint32_t
passive_read(struct bwfm_softc *sc, uint32_t reg)
{
	return 0;
}

static void
passive_write(struct bwfm_softc *sc, uint32_t reg, uint32_t val)
{
}

static struct bwfm_buscore_ops passive_buscore = {
	.bc_read = passive_read,
	.bc_write = passive_write,
};

static int
passive_intr(void *arg)
{
	return 1;
}

static void
passive_core(struct bwfm_softc *sc, uint16_t id)
{
	struct bwfm_core *core = malloc(sizeof(*core), M_DEVBUF, M_WAITOK | M_ZERO);

	core->co_id = id;
	LIST_INSERT_HEAD(&sc->sc_chip.ch_list, core, co_link);
}

/*
 * Detach leaves the chip passive in brcmfmac's order, with the bus
 * claimed, after the interrupt and the bus work and before bwfm forgets
 * the chip's cores. Not when the host set the card up again since the
 * last load: before the detach, during the interrupt's removal or during
 * the bus work's last pass, after the reset callback is gone. Nor on a
 * card given up.
 */
static void
passive_set_up(void)
{
	fake_sdio.generation++;
}

static void
test_detach_passive(void)
{
	static const char *const want[] = {
		"reset-off irq-off stop clocks-on delay-20000 cr4-halt d11-off clocks-off disable-2 disable-1 free",
		"reset-off irq-off stop free",
	};
	struct bwfm_softc *sc;
	struct bwfm_pie *p;
	long live = host_live();
	int c;

	for (c = 0; c < 5; c++) {
		p = join_setup(&sc);
		LIST_INIT(&sc->sc_chip.ch_list);
		passive_core(sc, BWFM_AGENT_CORE_80211);
		passive_core(sc, BWFM_AGENT_CORE_ARM_CR4);
		sc->sc_buscore_ops = &passive_buscore;
		sc->sc_chip.ch_core_reset = passive_core_reset;
		sc->sc_chip.ch_core_disable = passive_core_disable;
		p->bus_clocks = passive_clocks;
		p->sf[2].pf = FAKE_F2;
		p->intr_fn = passive_intr;
		p->attached = 1;
		p->reset_on = 1;
		p->generation = 7;
		fake_sdio.generation = c == 1 ? 8 : c == 4 ? 0 : 7;
		fake_sdio.set_up_on_unregister = c == 2;
		fake_stop_hook = c == 3 ? passive_set_up : NULL;
		passive_claimed = 1;
		fake_sdio.claims = fake_sdio.releases = 0;
		fake_log[0] = '\0';
		bwfm_pie_detach(p);
		fake_stop_hook = NULL;
		fake_sdio.set_up_on_unregister = 0;
		CHECK(strcmp(fake_log, want[c != 0]) == 0);
		CHECK(passive_claimed && fake_sdio.claims == fake_sdio.releases);
		CHECK(p->sc == NULL && p->os == NULL);
		bwfm_pie_delete(p);
		ml_purge(&up);
		ml_purge(&txq);
		fake_reset();
	}
	fake_sdio.generation = 0;
	CHECK(host_live() == live);
}

/*
 * Every attach has the card set up again, with the bus claimed, before
 * the chip's set-up. A card that does not come back fails the attach
 * with no other I/O.
 */
static void
test_attach_reset(void)
{
	struct bwfm_pie *p;
	long live = host_live();

	memset(&fake_sdio, 0, sizeof(fake_sdio));
	p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	CHECK(bwfm_pie_attach(p) == EIO && p->sc == NULL);
	CHECK(fake_sdio.ncalls > 1 && fake_sdio.calls[0].op == FAKE_RESET && fake_sdio.calls[0].fn == 1);
	CHECK(fake_sdio.resets_claimed == 1 && fake_sdio.claims == fake_sdio.releases);
	CHECK(fake_sdio.generation == 1 && p->generation == 1);
	bwfm_pie_delete(p);
	CHECK(host_live() == live);

	memset(&fake_sdio, 0, sizeof(fake_sdio));
	fake_sdio.reset_error = ENXIO;
	p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	CHECK(bwfm_pie_attach(p) == EIO && p->sc == NULL && p->drv == NULL);
	CHECK(fake_sdio.ncalls == 1 && fake_sdio.calls[0].op == FAKE_RESET);
	CHECK(fake_sdio.claims == fake_sdio.releases);
	bwfm_pie_delete(p);
	memset(&fake_sdio, 0, sizeof(fake_sdio));
	CHECK(host_live() == live);
}

static void
fake_attachhook(struct device *dev)
{
	((struct bwfm_softc *)dev)->sc_initialized = 1;
}

/* The chip's SDIO set-up faked: a softc as new_softc and ic_setup make it, then the mount hook. */
static void
fake_bus_attach(struct device *parent, struct device *self, void *aux)
{
	struct bwfm_softc *sc = (struct bwfm_softc *)self;

	TAILQ_INIT(&sc->sc_bcdc_rxctlq);
	sc->sc_bus_ops = &fake_bus;
	sc->sc_proto_ops = &bwfm_proto_bcdc_ops;
	ic_setup(sc);
	sc->sc_initialized = 0;
	config_mountroot(self, fake_attachhook);
}

/*
 * The real attach, with only the chip's SDIO set-up faked, then a reset of
 * the card: the attach keeps its count after reset_card as the last one
 * handled, so the callback for the next count handles it.
 */
static void
test_attach_then_reset(void)
{
	static const struct pie80211_drv_host host = { h_input, h_link, h_restart, NULL, NULL };
	struct bwfm_pie *p;
	long live = host_live();

	memset(&fake_sdio, 0, sizeof(fake_sdio));
	fake_sdio.generation = 6;
	p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	p->bus_attach = fake_bus_attach;
	p->bus_reset = fake_bus_reset;
	bus_resets = bus_reset_error = 0;
	CHECK(bwfm_pie_attach(p) == 0 && p->drv != NULL && p->reset_on && fake_sdio.reset_handler != NULL);
	CHECK(fake_sdio.generation == 7 && p->generation == 7 && p->reset_seen == 7 && p->resets == 0);
	if (p->drv != NULL && fake_sdio.reset_handler != NULL) {
		restart_drv = p->drv;
		pie80211_drv_set_host(p->drv, &host);
		restarts = 0;
		fake_sdio.generation = 8;
		fake_sdio.reset_handler(fake_sdio.reset_arg);
		CHECK(p->resets == 1 && p->reset_seen == 8 && p->bus_off && p->reload);
		pump();
		CHECK(restarts == 1 && bus_resets == 1 && !p->bus_off && !p->reload);
	}
	bwfm_pie_delete(p);
	ml_purge(&up);
	ml_purge(&txq);
	fake_reset();
	memset(&fake_sdio, 0, sizeof(fake_sdio));
	CHECK(host_live() == live);
}

static void
test_attach_fail(void)
{
	struct bwfm_pie *p;
	long live = host_live();

	fake_sdio.no_f2 = true;
	p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	CHECK(bwfm_pie_attach(p) == EIO && p->sc == NULL && p->drv == NULL);
	bwfm_pie_delete(p);
	fake_sdio.no_f2 = false;
	CHECK(host_live() == live);

	p = bwfm_pie_new(FAKE_F1, ROOT, NODE);
	CHECK(bwfm_pie_attach(p) == EIO && p->sc == NULL && p->drv == NULL);
	bwfm_pie_delete(p);
	CHECK(host_live() == live);
}

int
main(void)
{
	test_shim();
	test_nvram();
	test_firmware();
	test_blobs();
	test_txctl();
	test_scan();
	test_country();
	test_mac();
	test_join();
	test_join_fallback();
	test_handshake_ended();
	test_link_cache();
	test_refusals();
	test_key_refused();
	test_reset();
	test_detach();
	test_detach_key();
	test_detach_passive();
	test_reset_window();
	test_reset_order();
	test_attach_reset();
	test_attach_then_reset();
	test_attach_fail();
	return host_report("test_bwfm");
}
