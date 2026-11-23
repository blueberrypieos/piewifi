/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * What OpenBSD's bwfm and its SDIO glue need beyond PIE80211Family's
 * compat headers, on PIE. The compat headers PIEbwfm/compat stands in for
 * OpenBSD's sys/device.h, sys/buf.h, sys/pool.h, machine/fdt.h,
 * dev/ofw/openfirm.h and dev/sdmmc's headers; each includes this one.
 *
 * Threads: net80211 and everything that touches it runs in the family's
 * gate, including bwfm's task queue, which is the family's. The SDIO bus
 * work runs on PIEbwfm's bus thread, outside the gate, with the bus
 * claimed. It answers control requests itself, and hands event and data
 * frames to a task in the gate. bwfm_pie_lock() guards what both sides
 * touch: the transmit queue, its count and the control requests.
 */
#ifndef _BWFM_PIE_H
#define _BWFM_PIE_H

#include <pie80211/compat.h>

struct bwfm_softc;
struct pie_sdio_func;

#define M_WAITOK		0
#define M_TEMP			M_DEVBUF
#define PR_WAITOK		0
#define IPL_SOFTNET		0
#define MT_CONTROL		2
#define SEC_TO_NSEC(s)		((uint64_t)(s) * 1000000000ULL)
#define MSEC_TO_NSEC(ms)	((uint64_t)(ms) * 1000000ULL)

#define IFF_BROADCAST		0x0100
#define IFF_MULTICAST		0x0200
#define IFF_ALLMULTI		0x0400
#define IFF_PROMISC		0x0800

#define DVACT_QUIESCE		2
#define DVACT_WAKEUP		5

struct ether_addr {
	uint8_t ether_addr_octet[ETHER_ADDR_LEN];
};

typedef uintptr_t vaddr_t;

#ifndef le16toh
#define le16toh(x)			letoh16(x)
#define le32toh(x)			letoh32(x)
#endif

#define ml_empty(ml)			((ml)->mh_count == 0)
#define MBUF_LIST_FIRST(ml)		((ml)->mh_head)
#define ml_init(ml)			(*(ml) = (struct mbuf_list)MBUF_LIST_INITIALIZER())
#define taskq_barrier(tq)		((void)(tq))

#define mallocarray(n, size, type, flags)	bwfm_pie_mallocarray(n, size, type, flags)
#define dma_alloc(size, flags)			malloc(size, M_DEVBUF, M_WAITOK)
#define dma_free(p, size)			free(p, M_DEVBUF, size)
#define delay(us)				bwfm_pie_delay(us)
void *bwfm_pie_mallocarray(size_t n, size_t size, int type, int flags);
void bwfm_pie_delay(u_int us);

#define taskq_create(name, nthreads, ipl, flags)	systq
#define taskq_destroy(tq)			((void)(tq))

#define ifq_empty(q)			((q)->q_pkts.mh_count == 0)
#define ifq_dequeue(q)			pie80211_ifq_dequeue(q)
#define ifq_is_oactive(q)		((q)->q_oactive != 0)
#define ifq_set_oactive(q)		((q)->q_oactive = 1)
#define ifq_clr_oactive(q)		((q)->q_oactive = 0)
#define if_setlladdr(ifp, addr)		\
	(memcpy(((struct arpcom *)(ifp))->ac_enaddr, (addr), ETHER_ADDR_LEN), 0)

struct device {
	char dv_xname[16];
	struct device *dv_parent;
};
struct cfdriver {
	void *cd_devs;
	const char *cd_name;
	int cd_class;
};
#define DV_IFNET			1
struct cfattach {
	size_t ca_devsize;
	int (*ca_match)(struct device *, void *, void *);
	void (*ca_attach)(struct device *, struct device *, void *);
	int (*ca_detach)(struct device *, int);
};
#define config_mountroot(dev, fn)	bwfm_pie_mountroot(dev, fn)
void bwfm_pie_mountroot(struct device *dev, void (*fn)(struct device *));

struct rwlock {
	struct pie_sdio_func *rw_func;
};
#define rw_enter_write(rw)		bwfm_pie_rw_enter(rw)
#define rw_exit(rw)			bwfm_pie_rw_exit(rw)
#define rw_assert_wrlock(rw)		((void)(rw))
void bwfm_pie_rw_enter(struct rwlock *rw);
void bwfm_pie_rw_exit(struct rwlock *rw);

struct sdmmc_function {
	int number;
	struct pie_sdio_func *pf;
	void *cookie;
	void *sc;
};
struct sdmmc_attach_args {
	struct sdmmc_function *sf;
};
uint8_t sdmmc_io_read_1(struct sdmmc_function *sf, int reg);
void sdmmc_io_write_1(struct sdmmc_function *sf, int reg, uint8_t val);
uint32_t sdmmc_io_read_4(struct sdmmc_function *sf, int reg);
void sdmmc_io_write_4(struct sdmmc_function *sf, int reg, uint32_t val);
int sdmmc_io_read_multi_1(struct sdmmc_function *sf, int reg, u_char *data, int len);
int sdmmc_io_read_region_1(struct sdmmc_function *sf, int reg, u_char *data, int len);
int sdmmc_io_write_region_1(struct sdmmc_function *sf, int reg, u_char *data, int len);
int sdmmc_io_set_blocklen(struct sdmmc_function *sf, int len);
int sdmmc_io_function_enable(struct sdmmc_function *sf);
void sdmmc_io_function_disable(struct sdmmc_function *sf);
void *sdmmc_intr_establish(struct device *dev, int (*fn)(void *), void *arg, const char *name);
void sdmmc_intr_enable(struct sdmmc_function *sf);
struct rwlock *bwfm_pie_sdio_lock(struct sdmmc_function *sf);
struct sdmmc_function **bwfm_pie_sdio_functions(struct sdmmc_function *sf);

int OF_peer(int node);
int OF_getprop(int node, const char *name, void *buf, int len);
int OF_getproplen(int node, const char *name);
#define fdt_intr_establish(node, ipl, fn, arg, name)	((void *)NULL)
#define fdt_intr_enable(ih)				((void)(ih))
#define fdt_intr_disable(ih)				((void)(ih))

#define loadfirmware(name, bufp, lenp)	bwfm_pie_loadfirmware(name, bufp, lenp)
int bwfm_pie_loadfirmware(const char *name, u_char **bufp, size_t *lenp);
void bwfm_pie_free_blobs(struct bwfm_softc *sc);

#define wakeup(chan)			bwfm_pie_wakeup(chan)
void bwfm_pie_lock(struct bwfm_softc *sc);
void bwfm_pie_unlock(struct bwfm_softc *sc);
int bwfm_pie_sleep(struct bwfm_softc *sc, void *chan, uint64_t nsec);
void bwfm_pie_wakeup(void *chan);
void bwfm_pie_bus_kick(struct bwfm_softc *sc);
void bwfm_pie_rx_defer(struct bwfm_softc *sc, struct mbuf *m);
void bwfm_pie_tx_restart(struct bwfm_softc *sc);
int bwfm_pie_check_mac(struct bwfm_softc *sc);
void bwfm_pie_key_failed(struct bwfm_softc *sc);
int bwfm_sdio_pie_reset(struct bwfm_softc *sc);
void bwfm_sdio_pie_clocks(struct bwfm_softc *sc, int on);

#endif
