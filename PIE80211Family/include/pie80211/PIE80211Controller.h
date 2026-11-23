/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * The Ethernet controller of an 802.11 station, on a driver's
 * pie80211_drv. Its work loop is the family's gate. A driver subclasses
 * it: start() calls PIE80211Controller::start() first, then attaches the
 * driver, which attaches its pie80211_drv, then calls attachDriver().
 * stop() calls detachDriver(), then detaches the driver, then calls
 * PIE80211Controller::stop(). Build with __PRIVATE_SPI__, as
 * IONetworkingFamily is.
 */
#ifndef _PIE80211_CONTROLLER_H
#define _PIE80211_CONTROLLER_H

#ifndef __PRIVATE_SPI__
#error "PIE80211Controller.h needs __PRIVATE_SPI__, as IONetworkingFamily is built with it"
#endif

#include <IOKit/IOWorkLoop.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma clang diagnostic ignored "-Wshadow-field"
#pragma clang diagnostic ignored "-Wdocumentation"
#pragma clang diagnostic ignored "-Wdocumentation-html"
#include <sys/kpi_mbuf.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#pragma clang diagnostic pop

struct pie80211_drv;
struct pie80211_mbuf;

class PIE80211Controller : public IOEthernetController
{
	OSDeclareDefaultStructors(PIE80211Controller);

	IOWorkLoop *fWorkLoop;
	IOOutputQueue *fOutputQueue;
	IONetworkInterface *fNetif;
	IONetworkMedium *fMedium;
	struct pie80211_drv *fDrv;
	bool fEnabled;
	bool fGate;

	static void gate(void *ctx, void (*fn)(void *), void *arg);
	static bool inGate(void *ctx);
	static IOReturn gateCall(void *target, void *call, void *, void *, void *);
	static IOReturn attachGated(void *target, void *drv, void *, void *, void *);
	static IOReturn detachGated(void *target, void *, void *, void *, void *);
	static IOReturn commandGated(void *target, void *cmd, void *data, void *len, void *);
	static void hostInput(void *ctx, struct pie80211_mbuf *m);
	static void hostLink(void *ctx, int up, uint64_t baudrate);
	static void hostRestart(void *ctx);
	static void hostTxWake(void *ctx);

protected:
	bool attachDriver(struct pie80211_drv *drv);
	void detachDriver(void);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual bool createWorkLoop(void) APPLE_KEXT_OVERRIDE;
	virtual IOWorkLoop *getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
	virtual IOOutputQueue *createOutputQueue(void) APPLE_KEXT_OVERRIDE;
	virtual IONetworkInterface *createInterface(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn enable(IONetworkInterface *interface) APPLE_KEXT_OVERRIDE;
	virtual IOReturn disable(IONetworkInterface *interface) APPLE_KEXT_OVERRIDE;
	virtual UInt32 outputPacket(mbuf_t m, void *param) APPLE_KEXT_OVERRIDE;
	virtual IOReturn getHardwareAddress(IOEthernetAddress *addr) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) APPLE_KEXT_OVERRIDE;

	int driverCommand(uint32_t cmd, void *data, size_t len);
};

#endif
