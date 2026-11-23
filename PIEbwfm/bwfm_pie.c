/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * OpenBSD's sdmmc, firmware, device tree and autoconf calls of bwfm, on
 * <pie/sdio.h>, PIE80211Family and the platform calls of
 * bwfm_pie_priv.h; the attach and detach of a bwfm on an SDIO card.
 */
#include "bwfm_pie_priv.h"
#include <net80211/ieee80211_var.h>
#include <dev/ic/bwfmreg.h>
#include <dev/ic/bwfmvar.h>
#include <pie/sdio.h>
#include <pie80211/driver.h>
#include <pie80211/fullmac.h>
#include <pie80211/ioctl.h>
#include <pie80211/platform.h>
#include "crypto/sha2.h"

struct bwfm_sdio_softc;

extern const struct cfattach bwfm_sdio_ca;
void bwfm_sdio_task_locked(struct bwfm_sdio_softc *);
void bwfm_init(struct ifnet *);
void bwfm_stop(struct ifnet *);
int bwfm_fwvar_cmd_get_data(struct bwfm_softc *, int, void *, size_t);
int bwfm_fwvar_cmd_get_int(struct bwfm_softc *, int, uint32_t *);
int bwfm_fwvar_var_get_data(struct bwfm_softc *, char *, void *, size_t);
int bwfm_fwvar_var_set_data(struct bwfm_softc *, char *, void *, size_t);
static void bwfm_pie_link_forget(struct bwfm_pie *p);

static const struct {
	const char *name;
	size_t size;
	uint8_t sha256[SHA256_DIGEST_LENGTH];
} bwfm_pie_fw[] = {
	{ "brcmfmac43455-sdio.bin", 616233, {
	    0xea, 0xff, 0x8d, 0x2b, 0x6d, 0x25, 0x01, 0xbb, 0x5c, 0x47, 0x7b, 0xa3, 0x43, 0x90, 0x0c, 0x74,
	    0x87, 0xaf, 0x91, 0x58, 0x98, 0xea, 0xc1, 0x3b, 0xc9, 0x1b, 0x33, 0xb1, 0x28, 0x5d, 0xad, 0xce } },
	{ "brcmfmac43455-sdio.clm_blob", 4733, {
	    0x8f, 0xbe, 0x9f, 0xc2, 0x95, 0x2e, 0x2f, 0xba, 0xb0, 0x62, 0xa1, 0x42, 0xc1, 0xea, 0x3e, 0x26,
	    0x1c, 0xd7, 0x46, 0x04, 0x76, 0x1e, 0x12, 0xf3, 0x04, 0x78, 0x1b, 0x91, 0x1d, 0xf4, 0xa3, 0x28 } },
	{ "brcmfmac43455-sdio.raspberrypi,4-model-b.txt", 1883, {
	    0xed, 0xb6, 0xf4, 0xe4, 0xfb, 0x19, 0xe1, 0x89, 0x40, 0x00, 0x41, 0x24, 0xfe, 0xb4, 0xff, 0xe1,
	    0x60, 0xd7, 0x2f, 0xc6, 0x07, 0x24, 0x3a, 0x07, 0xa4, 0x48, 0x03, 0x38, 0xa2, 0x8b, 0x27, 0x48 } },
	{ "brcmfmac43455-sdio.raspberrypi,500.txt", 1883, {
	    0xed, 0xb6, 0xf4, 0xe4, 0xfb, 0x19, 0xe1, 0x89, 0x40, 0x00, 0x41, 0x24, 0xfe, 0xb4, 0xff, 0xe1,
	    0x60, 0xd7, 0x2f, 0xc6, 0x07, 0x24, 0x3a, 0x07, 0xa4, 0x48, 0x03, 0x38, 0xa2, 0x8b, 0x27, 0x48 } },
};

#define BWFM_PIE_NODES		4
#define BWFM_PIE_RELOAD_TRIES	3
#define BWFM_PIE_LINK_NS	1000000000ULL
#define BWFM_PIE_LINK_RETRY_NS	10000000000ULL
static void *bwfm_pie_nodes[BWFM_PIE_NODES];
static uint8_t bwfm_pie_nvram_mac[ETHER_ADDR_LEN];
static int bwfm_pie_nvram_has_mac;


void *
bwfm_pie_mallocarray(size_t n, size_t size, int type, int flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return malloc(n * size, type, flags);
}

void
bwfm_pie_delay(u_int us)
{
	bwfm_pie_os_delay(us);
}

void
bwfm_pie_mountroot(struct device *dev, void (*fn)(struct device *))
{
	struct bwfm_pie *p = ((struct bwfm_softc *)dev)->sc_pie;

	p->mount_dev = dev;
	p->mount_fn = fn;
	p->attached = 1;
}

void
bwfm_pie_rw_enter(struct rwlock *rw)
{
	(void)pie_sdio_claim(rw->rw_func);
}

void
bwfm_pie_rw_exit(struct rwlock *rw)
{
	pie_sdio_release(rw->rw_func);
}

void
bwfm_pie_lock(struct bwfm_softc *sc)
{
	bwfm_pie_os_lock(sc->sc_pie->os);
}

void
bwfm_pie_unlock(struct bwfm_softc *sc)
{
	bwfm_pie_os_unlock(sc->sc_pie->os);
}

int
bwfm_pie_sleep(struct bwfm_softc *sc, void *chan, uint64_t nsec)
{
	if (sc->sc_pie->bus_off)
		return 1;
	return bwfm_pie_os_sleep(sc->sc_pie->os, chan, nsec);
}

void
bwfm_pie_wakeup(void *chan)
{
	bwfm_pie_os_wakeup(chan);
}

void
bwfm_pie_bus_kick(struct bwfm_softc *sc)
{
	bwfm_pie_os_kick(sc->sc_pie->os);
}


void
bwfm_pie_rx_defer(struct bwfm_softc *sc, struct mbuf *m)
{
	struct bwfm_pie *p = sc->sc_pie;
	int queued = 0;

	bwfm_pie_os_lock(p->os);
	if (p->rxq.mh_count < BWFM_PIE_RXQ_MAX) {
		ml_enqueue(&p->rxq, m);
		queued = 1;
	}
	bwfm_pie_os_unlock(p->os);
	if (queued)
		task_add(systq, &p->rx_task);
	else
		m_freem(m);
}

static void
bwfm_pie_rx_task(void *arg)
{
	struct bwfm_pie *p = arg;
	struct bwfm_softc *sc = p->sc;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m;

	for (;;) {
		bwfm_pie_os_lock(p->os);
		m = ml_dequeue(&p->rxq);
		bwfm_pie_os_unlock(p->os);
		if (m == NULL)
			break;
		sc->sc_proto_ops->proto_rx(sc, m, &ml);
	}
	if (p->drv != NULL)
		if_input(&sc->sc_ic.ic_if, &ml);
	else
		ml_purge(&ml);
}

void
bwfm_pie_tx_restart(struct bwfm_softc *sc)
{
	task_add(systq, &sc->sc_pie->start_task);
}

static void
bwfm_pie_start_task(void *arg)
{
	struct bwfm_pie *p = arg;
	struct ifnet *ifp = &p->sc->sc_ic.ic_if;

	ifq_clr_oactive(&ifp->if_snd);
	if_start(ifp);
}

static int
bwfm_pie_bus_on(struct bwfm_pie *p)
{
	int on;

	bwfm_pie_os_lock(p->os);
	on = !p->bus_off;
	bwfm_pie_os_unlock(p->os);
	return on;
}

static void
bwfm_pie_bus(void *arg)
{
	struct bwfm_pie *p = arg;

	(void)pie_sdio_claim(p->f1);
	if (bwfm_pie_bus_on(p))
		bwfm_sdio_task_locked((struct bwfm_sdio_softc *)(void *)p->sc);
	pie_sdio_release(p->f1);
}


uint8_t
sdmmc_io_read_1(struct sdmmc_function *sf, int reg)
{
	uint8_t v = 0;

	(void)pie_sdio_read1(sf->pf, (unsigned int)sf->number, (uint32_t)reg, &v);
	return v;
}

void
sdmmc_io_write_1(struct sdmmc_function *sf, int reg, uint8_t val)
{
	(void)pie_sdio_write1(sf->pf, (unsigned int)sf->number, (uint32_t)reg, val);
}

uint32_t
sdmmc_io_read_4(struct sdmmc_function *sf, int reg)
{
	uint32_t v = 0;

	(void)pie_sdio_read4(sf->pf, (uint32_t)reg, &v);
	return v;
}

void
sdmmc_io_write_4(struct sdmmc_function *sf, int reg, uint32_t val)
{
	(void)pie_sdio_write4(sf->pf, (uint32_t)reg, val);
}

int
sdmmc_io_read_multi_1(struct sdmmc_function *sf, int reg, u_char *data, int len)
{
	return pie_sdio_read(sf->pf, (uint32_t)reg, data, (size_t)len, false);
}

int
sdmmc_io_read_region_1(struct sdmmc_function *sf, int reg, u_char *data, int len)
{
	return pie_sdio_read(sf->pf, (uint32_t)reg, data, (size_t)len, true);
}

int
sdmmc_io_write_region_1(struct sdmmc_function *sf, int reg, u_char *data, int len)
{
	return pie_sdio_write(sf->pf, (uint32_t)reg, data, (size_t)len, true);
}

int
sdmmc_io_set_blocklen(struct sdmmc_function *sf, int len)
{
	return pie_sdio_set_block_size(sf->pf, (unsigned int)len);
}

int
sdmmc_io_function_enable(struct sdmmc_function *sf)
{
	return pie_sdio_enable(sf->pf);
}

void
sdmmc_io_function_disable(struct sdmmc_function *sf)
{
	(void)pie_sdio_disable(sf->pf);
}

void *
sdmmc_intr_establish(struct device *dev, int (*fn)(void *), void *arg, const char *name)
{
	struct bwfm_pie *p = ((struct bwfm_softc *)arg)->sc_pie;

	p->intr_fn = fn;
	p->intr_arg = arg;
	return p;
}

static void
bwfm_pie_intr(void *arg)
{
	struct bwfm_pie *p = arg;

	if (p->intr_fn != NULL && bwfm_pie_bus_on(p))
		(void)p->intr_fn(p->intr_arg);
}

void
sdmmc_intr_enable(struct sdmmc_function *sf)
{
	struct bwfm_pie *p = sf->sc;
	int error = pie_sdio_interrupt_register(sf->pf, bwfm_pie_intr, p);

	if (error != 0)
		printf("%s: the card's interrupt: error %d\n", p->sc->sc_dev.dv_xname, error);
}

struct rwlock *
bwfm_pie_sdio_lock(struct sdmmc_function *sf)
{
	return &((struct bwfm_pie *)sf->sc)->lock;
}

struct sdmmc_function **
bwfm_pie_sdio_functions(struct sdmmc_function *sf)
{
	struct bwfm_pie *p = sf->sc;
	struct sdmmc_function **v;

	p->sf[2].pf = pie_sdio_function(p->f1, 2);
	if (p->sf[2].pf == NULL)
		return NULL;
	v = malloc(3 * sizeof(*v), M_DEVBUF, M_WAITOK);
	if (v != NULL) {
		v[0] = &p->sf[0];
		v[1] = &p->sf[1];
		v[2] = &p->sf[2];
	}
	return v;
}


int
bwfm_pie_of_add(void *osnode)
{
	int i;

	for (i = 0; i < BWFM_PIE_NODES; i++) {
		if (bwfm_pie_nodes[i] == NULL || bwfm_pie_nodes[i] == osnode) {
			bwfm_pie_nodes[i] = osnode;
			return i + 1;
		}
	}
	return 0;
}

void
bwfm_pie_of_clear(void)
{
	memset(bwfm_pie_nodes, 0, sizeof(bwfm_pie_nodes));
}

int
OF_peer(int node)
{
	return node == 0 && bwfm_pie_nodes[0] != NULL ? 1 : 0;
}

int
OF_getprop(int node, const char *name, void *buf, int len)
{
	if (node < 1 || node > BWFM_PIE_NODES || bwfm_pie_nodes[node - 1] == NULL)
		return -1;
	return bwfm_pie_os_getprop(bwfm_pie_nodes[node - 1], name, buf, len);
}

int
OF_getproplen(int node, const char *name)
{
	return OF_getprop(node, name, NULL, 0);
}


int
bwfm_pie_firmware_allowed(const char *name, const u_char *buf, size_t len)
{
	uint8_t digest[SHA256_DIGEST_LENGTH];
	SHA2_CTX ctx;
	size_t i;

	for (i = 0; i < nitems(bwfm_pie_fw); i++) {
		if (strcmp(name, bwfm_pie_fw[i].name) != 0)
			continue;
		if (buf == NULL)
			return 1;
		if (len != bwfm_pie_fw[i].size)
			return 0;
		SHA256Init(&ctx);
		SHA256Update(&ctx, buf, len);
		SHA256Final(digest, &ctx);
		return memcmp(digest, bwfm_pie_fw[i].sha256, sizeof(digest)) == 0;
	}
	return 0;
}

int
bwfm_pie_loadfirmware(const char *name, u_char **bufp, size_t *lenp)
{
	char path[128];
	u_char *buf;
	size_t len;
	int error;

	*bufp = NULL;
	*lenp = 0;
	if (!bwfm_pie_firmware_allowed(name, NULL, 0))
		return ENOENT;
	if (snprintf(path, sizeof(path), "%s%s", BWFM_PIE_FWDIR, name) >= (int)sizeof(path))
		return ENAMETOOLONG;
	error = bwfm_pie_os_readfile(path, &buf, &len);
	if (error != 0)
		return error;
	if (!bwfm_pie_firmware_allowed(name, buf, len)) {
		printf("bwfm: %s is not the file this driver knows; not used\n", path);
		free(buf, M_DEVBUF, len);
		return EINVAL;
	}
	if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".txt") == 0)
		bwfm_pie_nvram_has_mac = bwfm_pie_nvram_macaddr(buf, len, bwfm_pie_nvram_mac);
	*bufp = buf;
	*lenp = len;
	return 0;
}

int
bwfm_pie_nvram_macaddr(const u_char *buf, size_t len, uint8_t mac[ETHER_ADDR_LEN])
{
	static const char key[] = "macaddr=";
	unsigned int v[ETHER_ADDR_LEN];
	char text[18];
	size_t i;
	int n;

	for (i = 0; i + sizeof(key) - 1 + 17 <= len; i++) {
		if ((i > 0 && buf[i - 1] != '\n') || memcmp(buf + i, key, sizeof(key) - 1) != 0)
			continue;
		memcpy(text, buf + i + sizeof(key) - 1, 17);
		text[17] = '\0';
		if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4],
		    &v[5]) != ETHER_ADDR_LEN)
			return 0;
		for (n = 0; n < ETHER_ADDR_LEN; n++)
			mac[n] = (uint8_t)v[n];
		return 1;
	}
	return 0;
}

static int
bwfm_pie_mac_ok(const uint8_t mac[ETHER_ADDR_LEN], const uint8_t *shared)
{
	static const uint8_t zero[ETHER_ADDR_LEN];

	if (memcmp(mac, zero, ETHER_ADDR_LEN) == 0 || (mac[0] & 1) != 0)
		return 0;
	return shared == NULL || memcmp(mac, shared, ETHER_ADDR_LEN) != 0;
}

/*
 * The chip gets the address chosen at its first attach. Then that is its
 * own, unless it is zero, multicast or shared, the NVRAM's address of
 * every board. A random locally administered one takes its place, and is
 * read back.
 */
int
bwfm_pie_set_mac(struct bwfm_softc *sc, const uint8_t *shared)
{
	struct bwfm_pie *p = sc->sc_pie;
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t want[ETHER_ADDR_LEN], got[ETHER_ADDR_LEN];
	const char *why = NULL;

	if (p->has_mac) {
		memcpy(want, p->mac, sizeof(want));
	} else if (bwfm_pie_mac_ok(ic->ic_myaddr, shared)) {
		memcpy(want, ic->ic_myaddr, sizeof(want));
	} else {
		why = shared != NULL && memcmp(ic->ic_myaddr, shared, ETHER_ADDR_LEN) == 0 ?
		    "the NVRAM's shared address" : "no address of its own";
		pie80211_arc4random_buf(want, sizeof(want));
		want[0] = (want[0] & 0xfc) | 0x02;
	}
	if (!bwfm_pie_mac_ok(want, shared))
		return EINVAL;
	if (memcmp(want, ic->ic_myaddr, sizeof(want)) != 0) {
		memset(got, 0, sizeof(got));
		if (bwfm_fwvar_var_set_data(sc, "cur_etheraddr", want, sizeof(want)) != 0 ||
		    bwfm_fwvar_var_get_data(sc, "cur_etheraddr", got, sizeof(got)) != 0 ||
		    memcmp(got, want, sizeof(got)) != 0) {
			printf("%s: the chip did not take the address %s\n", sc->sc_dev.dv_xname,
			    ether_sprintf(want));
			return EIO;
		}
		if (why != NULL)
			printf("%s: the chip has %s; a random one instead\n", sc->sc_dev.dv_xname, why);
		memcpy(ic->ic_myaddr, want, sizeof(want));
	}
	memcpy(p->mac, want, sizeof(p->mac));
	p->has_mac = 1;
	return 0;
}

void
bwfm_pie_free_blobs(struct bwfm_softc *sc)
{
	free(sc->sc_clm, M_DEVBUF, sc->sc_clmsize);
	sc->sc_clm = NULL;
	sc->sc_clmsize = 0;
	free(sc->sc_txcap, M_DEVBUF, sc->sc_txcapsize);
	sc->sc_txcap = NULL;
	sc->sc_txcapsize = 0;
	free(sc->sc_cal, M_DEVBUF, sc->sc_calsize);
	sc->sc_cal = NULL;
	sc->sc_calsize = 0;
}

int
bwfm_pie_check_mac(struct bwfm_softc *sc)
{
	return bwfm_pie_set_mac(sc, bwfm_pie_nvram_has_mac ? bwfm_pie_nvram_mac : NULL);
}

void
bwfm_pie_key_failed(struct bwfm_softc *sc)
{
	struct bwfm_pie *p = sc->sc_pie;
	struct ieee80211com *ic = &sc->sc_ic;

	if (p->drv != NULL && bwfm_pie_bus_on(p))
		pie80211_drv_problem(p->drv, PIE80211_PROBLEM_KEY_INSTALL);
	if (ic->ic_state > IEEE80211_S_SCAN)
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
}



/*
 * A new set-up of the card: from the reset callback on the SDIO bus's
 * interrupt thread, or seen by its count after the attach. The count only
 * goes up, so one is handled only when it is past the last one handled,
 * compared across a wrap: a stale count, as the attach's check may hold,
 * is passed over, and a callback that reads a later count handles the
 * set-ups before it too. 0, the card given up, is handled once, and
 * nothing after it. The bus work stops, firmware commands fail at once,
 * and the family restarts the interface if it is up. The next init loads
 * the firmware again.
 */
static void
bwfm_pie_card_set_up(struct bwfm_pie *p, unsigned int gen)
{
	int restart;

	bwfm_pie_os_lock(p->os);
	if (p->reset_seen == 0 || (gen != 0 && (int)(gen - p->reset_seen) <= 0)) {
		bwfm_pie_os_unlock(p->os);
		return;
	}
	p->reset_seen = gen;
	p->resets++;
	p->bus_off = 1;
	p->reload = 1;
	restart = p->reload_fails < BWFM_PIE_RELOAD_TRIES;
	bwfm_pie_os_unlock(p->os);
	printf("%s: %s\n", p->sc->sc_dev.dv_xname, gen != 0 ? "the card was reset" : "the card was given up");
	if (restart && p->drv != NULL)
		pie80211_drv_restart(p->drv);
}

void
bwfm_pie_card_reset(void *arg)
{
	struct bwfm_pie *p = arg;

	bwfm_pie_card_set_up(p, pie_sdio_generation(p->f1));
}

/*
 * The reset callback set, then the card's set-up count read again, under
 * the claim: a new set-up after the attach's and before the callback was
 * there is seen only here. One that the callback sees as well is handled
 * once, as both go by the count, and a count that a callback has passed
 * meanwhile is not handled again.
 */
void
bwfm_pie_reset_watch(struct bwfm_pie *p)
{
	unsigned int gen;
	int error;

	error = pie_sdio_reset_register(p->f1, bwfm_pie_card_reset, p);
	if (error == 0)
		p->reset_on = 1;
	else
		printf("%s: no reset callback: error %d\n", p->sc->sc_dev.dv_xname, error);
	if (pie_sdio_claim(p->f1) != 0)
		return;
	gen = pie_sdio_generation(p->f1);
	pie_sdio_release(p->f1);
	if (gen != p->generation)
		bwfm_pie_card_set_up(p, gen);
}

/*
 * The chip set up as at attach, its firmware loaded and checked as at
 * attach, and the country set again. A reset of the card while this runs
 * fails it, and so does a country that does not go back.
 */
static int
bwfm_pie_reload(struct bwfm_pie *p)
{
	struct bwfm_softc *sc = p->sc;
	unsigned int resets;
	int error, country = 0;

	bwfm_pie_link_forget(p);
	bwfm_pie_os_lock(p->os);
	resets = p->resets;
	bwfm_pie_os_unlock(p->os);
	(void)pie_sdio_claim(p->f1);
	if (p->intr_fn != NULL) {
		(void)pie_sdio_interrupt_unregister(p->f1);
		p->intr_fn = NULL;
	}
	p->generation = pie_sdio_generation(p->f1);
	error = p->bus_reset(sc);
	pie_sdio_release(p->f1);
	sc->sc_cmdq.cur = sc->sc_cmdq.next = sc->sc_cmdq.queued = 0;
	sc->sc_key_tasks = 0;
	ml_purge(&sc->sc_evml);
	if (error == 0 && sc->sc_bus_ops->bs_preinit != NULL && sc->sc_bus_ops->bs_preinit(sc) != 0)
		error = EIO;
	bwfm_pie_os_lock(p->os);
	if (error == 0 && p->resets == resets)
		p->bus_off = 0;
	bwfm_pie_os_unlock(p->os);
	if (error == 0 && bwfm_preinit(sc) != 0)
		error = EIO;
	if (error == 0 && p->has_country && bwfm_pie_set_country(sc, p->country) != 0) {
		country = 1;
		error = EIO;
	}
	bwfm_pie_os_lock(p->os);
	if (error == 0 && p->resets == resets) {
		p->reload = 0;
		p->reload_fails = 0;
	} else {
		p->bus_off = 1;
		p->reload_fails++;
		error = EIO;
	}
	bwfm_pie_os_unlock(p->os);
	if (error != 0) {
		printf("%s: %s\n", sc->sc_dev.dv_xname, country ? "the country did not go back" :
		    "the firmware did not load again");
		return error;
	}
	sc->sc_initialized = 1;
	printf("%s: the firmware is loaded again\n", sc->sc_dev.dv_xname);
	return 0;
}

static void
bwfm_pie_init(void *softc)
{
	struct bwfm_softc *sc = softc;
	struct bwfm_pie *p = sc->sc_pie;
	int reload;

	bwfm_pie_os_lock(p->os);
	reload = p->reload;
	bwfm_pie_os_unlock(p->os);
	if (reload && bwfm_pie_reload(p) != 0)
		return;
	bwfm_init(&sc->sc_ic.ic_if);
}

static void
bwfm_pie_stop(void *softc)
{
	bwfm_stop(&((struct bwfm_softc *)softc)->sc_ic.ic_if);
}

struct bwfm_pie_country {
	char abbrev[4];
	uint32_t rev;
	char ccode[4];
};

int
bwfm_pie_set_country(void *softc, const char code[2])
{
	static const uint32_t revs[] = { 0xffffffff, 0 };
	struct bwfm_softc *sc = softc;
	struct bwfm_pie *p = sc->sc_pie;
	struct bwfm_pie_country c;
	size_t i;

	for (i = 0; i < nitems(revs); i++) {
		memset(&c, 0, sizeof(c));
		memcpy(c.abbrev, code, 2);
		memcpy(c.ccode, code, 2);
		c.rev = htole32(revs[i]);
		if (bwfm_fwvar_var_set_data(sc, "country", &c, sizeof(c)) != 0)
			continue;
		memcpy(p->country, code, sizeof(p->country));
		p->has_country = 1;
		memset(&c, 0, sizeof(c));
		if (bwfm_fwvar_var_get_data(sc, "country", &c, sizeof(c)) == 0)
			printf("%s: country %.2s, revision %d\n", sc->sc_dev.dv_xname, c.ccode,
			    (int)letoh32(c.rev));
		return 0;
	}
	return EIO;
}

struct bwfm_pie_scb {
	uint32_t val;
	uint8_t addr[ETHER_ADDR_LEN];
	uint8_t pad[2];
};

/*
 * The firmware's signal and rate, asked at most once a second, and after
 * a failure once in ten, as any user may ask for the status in a loop.
 * Any change of state, and a reload, drops what was asked.
 */
static void
bwfm_pie_link_forget(struct bwfm_pie *p)
{
	p->link_when = 0;
	p->link_error = 0;
}

static int
bwfm_pie_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct bwfm_softc *sc = ic->ic_softc;
	struct bwfm_pie *p = sc->sc_pie;

	bwfm_pie_link_forget(p);
	return p->newstate(ic, nstate, arg);
}

/* The family's driver for bwfm's net80211 state, with bwfm's newstate wrapped. */
struct pie80211_drv *
bwfm_pie_drv_attach(struct bwfm_pie *p, int *error)
{
	struct ieee80211com *ic = &p->sc->sc_ic;
	struct pie80211_drv *d;

	p->newstate = ic->ic_newstate;
	ic->ic_newstate = bwfm_pie_newstate;
	d = pie80211_drv_attach_ic(ic, &bwfm_pie_fmops, p->sc, error);
	if (d == NULL)
		ic->ic_newstate = p->newstate;
	return d;
}

int
bwfm_pie_link_info(void *softc, int8_t *rssi, uint32_t *rate_kbps)
{
	struct bwfm_softc *sc = softc;
	struct bwfm_pie *p = sc->sc_pie;
	struct bwfm_pie_scb scb;
	uint64_t now = pie80211_nsecuptime();
	uint32_t rate;
	int32_t v;

	if (p->link_when != 0 &&
	    now - p->link_when < (p->link_error ? BWFM_PIE_LINK_RETRY_NS : BWFM_PIE_LINK_NS)) {
		if (p->link_error)
			return EIO;
		*rssi = p->link_rssi;
		*rate_kbps = p->link_rate;
		return 0;
	}
	p->link_when = now;
	memset(&scb, 0, sizeof(scb));
	if (bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_RSSI, &scb, sizeof(scb)) != 0 ||
	    bwfm_fwvar_cmd_get_int(sc, BWFM_C_GET_RATE, &rate) != 0) {
		p->link_error = 1;
		return EIO;
	}
	v = (int32_t)letoh32(scb.val);
	p->link_error = 0;
	p->link_rssi = (int8_t)(v < -128 ? -128 : v > 0 ? 0 : v);
	p->link_rate = rate * 500;
	*rssi = p->link_rssi;
	*rate_kbps = p->link_rate;
	return 0;
}

const struct pie80211_fullmac_ops bwfm_pie_fmops = {
	.init = bwfm_pie_init,
	.stop = bwfm_pie_stop,
	.set_country = bwfm_pie_set_country,
	.link_info = bwfm_pie_link_info,
};

/*
 * The card is powered up and set up again first, every time: a chip that
 * a driver before this one left running, or left passive, does not answer
 * a new firmware.
 */
static void
bwfm_pie_attach_gated(void *arg)
{
	struct bwfm_pie *p = arg;
	struct sdmmc_attach_args saa = { &p->sf[1] };
	int error;

	(void)pie_sdio_claim(p->f1);
	error = pie_sdio_reset_card(p->f1);
	p->generation = pie_sdio_generation(p->f1);
	p->reset_seen = p->generation;
	if (error == 0)
		p->bus_attach(NULL, &p->sc->sc_dev, &saa);
	else
		printf("%s: the card was not set up again: error %d\n", p->sc->sc_dev.dv_xname, error);
	pie_sdio_release(p->f1);
	if (p->mount_fn != NULL)
		p->mount_fn(p->mount_dev);
}

static void
bwfm_pie_quiet_gated(void *arg)
{
	struct bwfm_pie *p = arg;

	if (p->intr_fn != NULL) {
		(void)pie_sdio_claim(p->f1);
		(void)pie_sdio_interrupt_unregister(p->f1);
		pie_sdio_release(p->f1);
		p->intr_fn = NULL;
	}
}

/*
 * Leave the chip inactive when the driver is removed: request the clock,
 * pause, halt the ARM core and reset the other cores, turn the clocks
 * off, then disable functions 2 and 1. Its firmware then does not
 * stay associated and active with no driver loaded. A card that the host
 * set up again since its last load is left as the new set-up left it,
 * whether or not the reset callback still saw it, and so is a card given
 * up and a chip whose cores bwfm does not know.
 */
static void
bwfm_pie_passive(struct bwfm_pie *p)
{
	struct bwfm_softc *sc = p->sc;

	if (LIST_EMPTY(&sc->sc_chip.ch_list) || pie_sdio_claim(p->f1) != 0)
		return;
	if (pie_sdio_generation(p->f1) != p->generation) {
		pie_sdio_release(p->f1);
		return;
	}
	p->bus_clocks(sc, 1);
	bwfm_pie_os_delay(20 * 1000);
	bwfm_chip_set_passive(sc);
	p->bus_clocks(sc, 0);
	if (p->sf[2].pf != NULL)
		(void)pie_sdio_disable(p->sf[2].pf);
	(void)pie_sdio_disable(p->f1);
	pie_sdio_release(p->f1);
}

/*
 * With the interrupt and the bus work stopped, nothing queues work for the
 * gate but the detach itself. The chip is left passive while bwfm still
 * knows its cores. bwfm's own detach may queue its task, to delete a key.
 */
static void
bwfm_pie_detach_gated(void *arg)
{
	struct bwfm_pie *p = arg;
	struct bwfm_softc *sc = p->sc;

	task_del(systq, &p->rx_task);
	task_del(systq, &p->start_task);
	ml_purge(&p->rxq);
	if (p->attached) {
		bwfm_pie_passive(p);
		(void)bwfm_sdio_ca.ca_detach(&sc->sc_dev, 0);
	}
	p->attached = 0;
	task_del(systq, &sc->sc_task);
}

int
bwfm_pie_attach(struct bwfm_pie *p)
{
	struct bwfm_softc *sc;
	int error, i;

	sc = malloc(bwfm_sdio_ca.ca_devsize, M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL)
		return ENOMEM;
	sc->sc_pie = p;
	p->sc = sc;
	strlcpy(sc->sc_dev.dv_xname, "bwfm0", sizeof(sc->sc_dev.dv_xname));
	p->lock.rw_func = p->f1;
	for (i = 0; i < 3; i++) {
		p->sf[i].number = i;
		p->sf[i].pf = p->f1;
		p->sf[i].sc = p;
	}
	p->sf[1].cookie = &p->node;
	p->os = bwfm_pie_os_alloc(bwfm_pie_bus, p);
	if (p->os == NULL) {
		free(sc, M_DEVBUF, bwfm_sdio_ca.ca_devsize);
		p->sc = NULL;
		return ENOMEM;
	}
	pie80211_gated(bwfm_pie_attach_gated, p);
	if (!p->attached || !sc->sc_initialized) {
		printf("%s: not attached\n", sc->sc_dev.dv_xname);
		bwfm_pie_detach(p);
		return EIO;
	}
	p->drv = bwfm_pie_drv_attach(p, &error);
	if (p->drv == NULL) {
		bwfm_pie_detach(p);
		return error;
	}
	bwfm_pie_reset_watch(p);
	return 0;
}

static void
bwfm_pie_drop_drv_gated(void *arg)
{
	struct bwfm_pie *p = arg;

	p->drv = NULL;
}

/*
 * The reset callback goes first. Then the gate's work stops seeing the
 * family's driver before the family frees it. Then the card's interrupt
 * and the bus work go. The gate's work for the card goes while its lock is
 * there, and the lock goes last.
 */
void
bwfm_pie_detach(struct bwfm_pie *p)
{
	struct pie80211_drv *d;

	if (p->sc == NULL)
		return;
	if (p->reset_on) {
		(void)pie_sdio_reset_register(p->f1, NULL, NULL);
		p->reset_on = 0;
	}
	if (p->drv != NULL) {
		d = p->drv;
		pie80211_gated(bwfm_pie_drop_drv_gated, p);
		pie80211_drv_detach(d);
	}
	pie80211_gated(bwfm_pie_quiet_gated, p);
	bwfm_pie_os_stop(p->os);
	pie80211_gated(bwfm_pie_detach_gated, p);
	bwfm_pie_os_free(p->os);
	p->os = NULL;
	free(p->sc, M_DEVBUF, bwfm_sdio_ca.ca_devsize);
	p->sc = NULL;
}

struct bwfm_pie *
bwfm_pie_new(struct pie_sdio_func *f1, void *root, void *node)
{
	struct bwfm_pie *p = malloc(sizeof(*p), M_DEVBUF, M_WAITOK | M_ZERO);

	if (p == NULL)
		return NULL;
	p->f1 = f1;
	p->rxq = (struct mbuf_list)MBUF_LIST_INITIALIZER();
	task_set(&p->rx_task, bwfm_pie_rx_task, p);
	task_set(&p->start_task, bwfm_pie_start_task, p);
	p->bus_attach = bwfm_sdio_ca.ca_attach;
	p->bus_reset = bwfm_sdio_pie_reset;
	p->bus_clocks = bwfm_sdio_pie_clocks;
	bwfm_pie_of_clear();
	if (root != NULL)
		(void)bwfm_pie_of_add(root);
	if (node != NULL)
		p->node = bwfm_pie_of_add(node);
	return p;
}

struct pie80211_drv *
bwfm_pie_start(struct bwfm_pie *p, int *error)
{
	*error = bwfm_pie_attach(p);
	return *error == 0 ? p->drv : NULL;
}

void
bwfm_pie_delete(struct bwfm_pie *p)
{
	bwfm_pie_detach(p);
	bwfm_pie_of_clear();
	free(p, M_DEVBUF, sizeof(*p));
}
