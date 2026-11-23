/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * A FullMAC driver whose firmware does the 802.11 work, such as OpenBSD's
 * bwfm, drives net80211 itself, as on OpenBSD. It builds with the family's
 * private headers, attaches its own ieee80211com and ifnet with if_attach()
 * and ieee80211_ifattach(), and sets their hooks. pie80211_drv_attach_ic()
 * then hands that ic to a PIE80211Controller: the result goes to
 * attachDriver(), and pie80211_drv_detach() undoes it. The driver detaches
 * its ic itself afterwards.
 *
 * The controller's up calls init, and down calls stop, in the gate. They
 * may sleep for the firmware. init sets IFF_RUNNING when it has
 * succeeded. Output goes to the ifnet's send queue and if_start. Frames
 * the driver passes to if_input, and its link state changes, reach the
 * controller. While the interface is up, the family counts down if_timer
 * once a second and calls if_watchdog when it reaches 0, as OpenBSD does.
 * set_country may be NULL. So may link_info: in RUN it gives the signal in
 * dBm and the transmit rate in kb/s that the firmware has for the network
 * joined, for the status command.
 *
 * Between attach and detach the family hooks the ic's newstate and set_key.
 * It refuses to join a network whose group cipher is not CCMP, as for any
 * driver, and refuses a key of another cipher. In the gate,
 * pie80211_drv_problem() reports a problem with the network being joined
 * for the status command. The link coming up clears it. As for any driver,
 * pie80211_drv_restart() of driver.h has the controller take an interface
 * that is up down and up again.
 *
 * pie80211_drv_attach_ic() is called outside the gate, after the driver's
 * firmware has given it the MAC address.
 */
#ifndef _PIE80211_FULLMAC_H
#define _PIE80211_FULLMAC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ieee80211com;
struct pie80211_drv;

struct pie80211_fullmac_ops {
	void (*init)(void *softc);
	void (*stop)(void *softc);
	int (*set_country)(void *softc, const char code[2]);
	int (*link_info)(void *softc, int8_t *rssi, uint32_t *rate_kbps);
};

struct pie80211_drv *pie80211_drv_attach_ic(struct ieee80211com *ic,
    const struct pie80211_fullmac_ops *ops, void *softc, int *error);
void pie80211_drv_problem(struct pie80211_drv *d, uint32_t problem);

#ifdef __cplusplus
}
#endif

#endif
