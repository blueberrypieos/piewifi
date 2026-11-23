/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/copyio.h>
#include <stdint.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sockio.h>
#include <sys/sockio_private.h>
#include <net/if_private.h>

#include <pie80211/PIE80211Controller.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma clang diagnostic ignored "-Wshadow-field"
#pragma clang diagnostic ignored "-Wdocumentation"
#pragma clang diagnostic ignored "-Wdocumentation-html"
#include <IOKit/network/IOGatedOutputQueue.h>
#pragma clang diagnostic pop

#include <pie80211/driver.h>
#include <pie80211/family_xnu.h>

#define PIE80211_OUTPUT_LEAD	64
#define PIE80211_OUTPUT_QUEUE	256

class PIE80211Interface : public IOEthernetInterface
{
	OSDeclareDefaultStructors(PIE80211Interface);

public:
	virtual SInt32 performCommand(IONetworkController *controller, unsigned long cmd, void *arg0,
	    void *arg1) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(PIE80211Interface, IOEthernetInterface);

SInt32
PIE80211Interface::performCommand(IONetworkController *controller, unsigned long cmd, void *arg0,
    void *arg1)
{
	PIE80211Controller *pc;
	struct ifdrv64 *ifd;
	void *buf = NULL;
	size_t len;
	proc_t p;
	int set, root, error;

	if (cmd != SIOCSDRVSPEC64 && cmd != SIOCGDRVSPEC64)
		return IOEthernetInterface::performCommand(controller, cmd, arg0, arg1);
	pc = OSDynamicCast(PIE80211Controller, controller);
	ifd = static_cast<struct ifdrv64 *>(arg1);
	if (pc == NULL || ifd == NULL)
		return EINVAL;
	if (ifd->ifd_cmd > UINT32_MAX ||
	    pie80211_drv_cmdinfo((uint32_t)ifd->ifd_cmd, &len, &set, &root) != 0)
		return EOPNOTSUPP;
	if ((set != 0) != (cmd == SIOCSDRVSPEC64) || ifd->ifd_len != len)
		return EINVAL;
	if (root) {
		p = proc_self();
		error = p != NULL ? proc_suser(p) : EPERM;
		if (p != NULL)
			proc_rele(p);
		if (error != 0)
			return EPERM;
	}
	if (len > 0 && (buf = IOMallocZero(len)) == NULL)
		return ENOMEM;
	error = 0;
	if (set && len > 0)
		error = copyin((user_addr_t)ifd->ifd_data, buf, len);
	if (error == 0)
		error = pc->driverCommand((uint32_t)ifd->ifd_cmd, buf, len);
	if (error == 0 && !set && len > 0)
		error = copyout(buf, (user_addr_t)ifd->ifd_data, len);
	if (buf != NULL) {
		(void)memset_s(buf, len, 0, len);
		IOFree(buf, len);
	}
	return error;
}

OSDefineMetaClassAndStructors(PIE80211Controller, IOEthernetController);

struct PIE80211GateCall {
	void (*fn)(void *);
	void *arg;
};

IOReturn
PIE80211Controller::gateCall(void *target, void *call, void *, void *, void *)
{
	PIE80211GateCall *c = static_cast<PIE80211GateCall *>(call);

	c->fn(c->arg);
	return kIOReturnSuccess;
}

void
PIE80211Controller::gate(void *ctx, void (*fn)(void *), void *arg)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(ctx);
	PIE80211GateCall c = { fn, arg };

	if (self->executeCommand(self, gateCall, self, &c) != kIOReturnSuccess)
		IOLog("%s: the gate refused a call\n", self->getName());
}

bool
PIE80211Controller::inGate(void *ctx)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(ctx);

	return self->fWorkLoop != NULL && self->fWorkLoop->inGate();
}

bool
PIE80211Controller::start(IOService *provider)
{
	if (!IOEthernetController::start(provider))
		return false;
	fMedium = IONetworkMedium::medium(kIOMediumEthernetAuto, 0);
	if (fMedium == NULL || fOutputQueue == NULL) {
		IOEthernetController::stop(provider);
		return false;
	}
	if (pie80211_set_gate(gate, inGate, this) != 0) {
		IOLog("%s: another 802.11 controller runs\n", getName());
		IOEthernetController::stop(provider);
		return false;
	}
	fGate = true;
	return true;
}

void
PIE80211Controller::stop(IOService *provider)
{
	detachDriver();
	if (fGate) {
		(void)pie80211_set_gate(NULL, NULL, NULL);
		fGate = false;
	}
	IOEthernetController::stop(provider);
}

void
PIE80211Controller::free(void)
{
	OSSafeReleaseNULL(fNetif);
	OSSafeReleaseNULL(fMedium);
	OSSafeReleaseNULL(fOutputQueue);
	OSSafeReleaseNULL(fWorkLoop);
	IOEthernetController::free();
}

bool
PIE80211Controller::createWorkLoop(void)
{
	fWorkLoop = IOWorkLoop::workLoop();
	return fWorkLoop != NULL;
}

IOWorkLoop *
PIE80211Controller::getWorkLoop(void) const
{
	return fWorkLoop;
}

IOOutputQueue *
PIE80211Controller::createOutputQueue(void)
{
	fOutputQueue = IOGatedOutputQueue::withTarget(this, getWorkLoop());
	if (fOutputQueue != NULL)
		fOutputQueue->retain();
	return fOutputQueue;
}

IONetworkInterface *
PIE80211Controller::createInterface(void)
{
	PIE80211Interface *netif = new PIE80211Interface;

	if (netif != NULL && !netif->init(this)) {
		netif->release();
		netif = NULL;
	}
	return netif;
}

IOReturn
PIE80211Controller::attachGated(void *target, void *drv, void *, void *, void *)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(target);
	struct pie80211_drv_host host = { hostInput, hostLink, hostRestart, hostTxWake, self };

	self->fDrv = static_cast<struct pie80211_drv *>(drv);
	pie80211_drv_set_host(self->fDrv, &host);
	return kIOReturnSuccess;
}

IOReturn
PIE80211Controller::detachGated(void *target, void *, void *, void *, void *)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(target);
	struct pie80211_drv_host none = { NULL, NULL, NULL, NULL, NULL };

	if (self->fOutputQueue != NULL) {
		self->fOutputQueue->stop();
		self->fOutputQueue->flush();
	}
	if (self->fDrv != NULL) {
		pie80211_drv_set_host(self->fDrv, &none);
		pie80211_drv_down(self->fDrv);
		self->fDrv = NULL;
	}
	self->fEnabled = false;
	return kIOReturnSuccess;
}

bool
PIE80211Controller::attachDriver(struct pie80211_drv *drv)
{
	OSDictionary *media;
	bool ok;

	if (drv == NULL || fDrv != NULL || fMedium == NULL)
		return false;
	(void)executeCommand(this, attachGated, this, drv);
	media = OSDictionary::withCapacity(1);
	ok = media != NULL && IONetworkMedium::addMedium(media, fMedium) &&
	    publishMediumDictionary(media);
	OSSafeReleaseNULL(media);
	ok = ok && setSelectedMedium(fMedium) && attachInterface(&fNetif, false);
	if (!ok) {
		(void)executeCommand(this, detachGated, this);
		OSSafeReleaseNULL(fNetif);
		return false;
	}
	setLinkStatus(kIONetworkLinkValid);
	fNetif->registerService();
	return true;
}

void
PIE80211Controller::detachDriver(void)
{
	(void)executeCommand(this, detachGated, this);
	if (fNetif != NULL) {
		detachInterface(fNetif, true);
		OSSafeReleaseNULL(fNetif);
	}
}

IOReturn
PIE80211Controller::enable(IONetworkInterface *interface)
{
	if (fEnabled)
		return kIOReturnSuccess;
	if (fDrv == NULL)
		return kIOReturnNotReady;
	if (pie80211_drv_up(fDrv) != 0)
		return kIOReturnIOError;
	fEnabled = true;
	setLinkStatus(kIONetworkLinkValid);
	fOutputQueue->setCapacity(PIE80211_OUTPUT_QUEUE);
	fOutputQueue->start();
	return kIOReturnSuccess;
}

IOReturn
PIE80211Controller::disable(IONetworkInterface *interface)
{
	if (!fEnabled)
		return kIOReturnSuccess;
	fEnabled = false;
	fOutputQueue->stop();
	fOutputQueue->flush();
	if (fDrv != NULL)
		pie80211_drv_down(fDrv);
	setLinkStatus(kIONetworkLinkValid);
	return kIOReturnSuccess;
}

UInt32
PIE80211Controller::outputPacket(mbuf_t m, void *param)
{
	struct pie80211_mbuf *pm;

	if (!fEnabled || fDrv == NULL) {
		freePacket(m);
		return kIOReturnOutputDropped;
	}
	if (pie80211_drv_tx_stopped(fDrv))
		return kIOReturnOutputStall;
	pm = pie80211_m_from_xnu(m, PIE80211_OUTPUT_LEAD);
	freePacket(m);
	if (pm == NULL || pie80211_drv_output(fDrv, pm) != 0)
		return kIOReturnOutputDropped;
	return kIOReturnOutputSuccess;
}

IOReturn
PIE80211Controller::getHardwareAddress(IOEthernetAddress *addr)
{
	if (fDrv == NULL)
		return kIOReturnNotReady;
	memcpy(addr->bytes, pie80211_drv_addr(fDrv), kIOEthernetAddressSize);
	return kIOReturnSuccess;
}

IOReturn
PIE80211Controller::setPromiscuousMode(bool active)
{
	return kIOReturnSuccess;
}

IOReturn
PIE80211Controller::setMulticastMode(bool active)
{
	return kIOReturnSuccess;
}

IOReturn
PIE80211Controller::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
	return kIOReturnSuccess;
}

IOReturn
PIE80211Controller::commandGated(void *target, void *cmd, void *data, void *len, void *)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(target);

	if (self->fDrv == NULL)
		return ENXIO;
	return pie80211_drv_ioctl(self->fDrv, (uint32_t)(uintptr_t)cmd, data, (size_t)(uintptr_t)len);
}

int
PIE80211Controller::driverCommand(uint32_t cmd, void *data, size_t len)
{
	IOReturn ret;

	ret = executeCommand(this, commandGated, this, (void *)(uintptr_t)cmd, data,
	    (void *)(uintptr_t)len);
	return ret < 0 ? EIO : (int)ret;
}

void
PIE80211Controller::hostInput(void *ctx, struct pie80211_mbuf *m)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(ctx);
	mbuf_t xm;

	if (pie80211_m_to_xnu(m, &xm) != 0)
		return;
	if (self->fNetif == NULL || !self->fEnabled) {
		mbuf_freem(xm);
		return;
	}
	(void)self->fNetif->inputPacket(xm);
}

void
PIE80211Controller::hostLink(void *ctx, int up, uint64_t baudrate)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(ctx);

	if (up)
		self->setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, self->fMedium, baudrate);
	else
		self->setLinkStatus(kIONetworkLinkValid);
}

void
PIE80211Controller::hostRestart(void *ctx)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(ctx);

	if (self->fDrv == NULL || !self->fEnabled)
		return;
	IOLog("%s: the driver restarted\n", self->getName());
	pie80211_drv_down(self->fDrv);
	(void)pie80211_drv_up(self->fDrv);
}

void
PIE80211Controller::hostTxWake(void *ctx)
{
	PIE80211Controller *self = static_cast<PIE80211Controller *>(ctx);

	if (self->fEnabled)
		(void)self->fOutputQueue->service(IOBasicOutputQueue::kServiceAsync);
}
