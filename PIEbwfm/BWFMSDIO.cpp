/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * BWFMSDIO: the CYW43455 of the Raspberry Pi 4 and 500 on function 1 of its
 * SDIO card, OpenBSD's bwfm on PIE80211Family. The boot-arg pie_wifi=0
 * leaves it out.
 */
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <pie80211/PIE80211Controller.h>
#include "bwfm_pie_kext.h"
#include "bwfm_pie_os.h"

class BWFMSDIO : public PIE80211Controller {
	OSDeclareDefaultStructors(BWFMSDIO);

	struct bwfm_pie *fPie;
	IORegistryEntry *fRoot;
	IORegistryEntry *fNode;

	void cleanup(void);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(BWFMSDIO, PIE80211Controller);

class BWFMSDIOWorker : public OSObject {
	OSDeclareDefaultStructors(BWFMSDIOWorker);

	IOWorkLoop *fLoop;
	IOInterruptEventSource *fSource;
	bool fAdded;
	void (*fFn)(void *);
	void *fArg;

	static void run(OSObject *owner, IOInterruptEventSource *source, int count);

public:
	static BWFMSDIOWorker *withFunction(void (*fn)(void *), void *arg);
	void kick(void);
	void stop(void);
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(BWFMSDIOWorker, OSObject);

BWFMSDIOWorker *
BWFMSDIOWorker::withFunction(void (*fn)(void *), void *arg)
{
	BWFMSDIOWorker *w = new BWFMSDIOWorker;

	if (w != NULL && !w->init())
		OSSafeReleaseNULL(w);
	if (w == NULL)
		return NULL;
	w->fFn = fn;
	w->fArg = arg;
	w->fLoop = IOWorkLoop::workLoop();
	if (w->fLoop != NULL)
		w->fSource = IOInterruptEventSource::interruptEventSource(w, run);
	if (w->fSource != NULL && w->fLoop->addEventSource(w->fSource) == kIOReturnSuccess)
		w->fAdded = true;
	if (!w->fAdded)
		OSSafeReleaseNULL(w);
	return w;
}

void
BWFMSDIOWorker::run(OSObject *owner, IOInterruptEventSource *source, int count)
{
	BWFMSDIOWorker *w = static_cast<BWFMSDIOWorker *>(owner);

	w->fFn(w->fArg);
}

void
BWFMSDIOWorker::kick(void)
{
	fSource->interruptOccurred(NULL, NULL, 0);
}

void
BWFMSDIOWorker::stop(void)
{
	if (fAdded) {
		(void)fLoop->removeEventSource(fSource);
		fAdded = false;
	}
}

void
BWFMSDIOWorker::free(void)
{
	stop();
	OSSafeReleaseNULL(fSource);
	OSSafeReleaseNULL(fLoop);
	OSObject::free();
}

extern "C" struct bwfm_pie_worker *
bwfm_pie_worker_new(void (*fn)(void *), void *arg)
{
	return reinterpret_cast<struct bwfm_pie_worker *>(BWFMSDIOWorker::withFunction(fn, arg));
}

extern "C" void
bwfm_pie_worker_kick(struct bwfm_pie_worker *w)
{
	reinterpret_cast<BWFMSDIOWorker *>(w)->kick();
}

extern "C" void
bwfm_pie_worker_stop(struct bwfm_pie_worker *w)
{
	reinterpret_cast<BWFMSDIOWorker *>(w)->stop();
}

extern "C" void
bwfm_pie_worker_free(struct bwfm_pie_worker *w)
{
	reinterpret_cast<BWFMSDIOWorker *>(w)->release();
}

/*
 * The device tree node of the chip: the child of the SDIO host's node whose
 * compatible is brcm,bcm4329-fmac. It is not a nub, so IODTCompareNubName()
 * reads its compatible.
 */
static IORegistryEntry *
chipNode(IOService *function)
{
	IOService *host = function->getProvider();
	IOService *slot = host != NULL ? host->getProvider() : NULL;
	OSIterator *it = slot != NULL ? slot->getChildIterator(gIODTPlane) : NULL;
	OSString *compat = OSString::withCString("brcm,bcm4329-fmac");
	IORegistryEntry *found = NULL;

	for (IORegistryEntry *c; it != NULL && compat != NULL && found == NULL &&
	    (c = OSDynamicCast(IORegistryEntry, it->getNextObject())) != NULL;) {
		if (IODTCompareNubName(c, compat, NULL)) {
			found = c;
			found->retain();
		}
	}
	OSSafeReleaseNULL(compat);
	OSSafeReleaseNULL(it);
	return found;
}

extern "C" int
bwfm_pie_os_getprop(void *osnode, const char *name, void *buf, int len)
{
	IORegistryEntry *e = static_cast<IORegistryEntry *>(osnode);
	OSData *d = OSDynamicCast(OSData, e->getProperty(name));

	if (d == NULL)
		return -1;
	if (buf != NULL && len > 0)
		memcpy(buf, d->getBytesNoCopy(), (unsigned int)len < d->getLength() ? (unsigned int)len : d->getLength());
	return (int)d->getLength();
}

void
BWFMSDIO::cleanup(void)
{
	if (fPie != NULL)
		bwfm_pie_delete(fPie);
	fPie = NULL;
	OSSafeReleaseNULL(fNode);
	OSSafeReleaseNULL(fRoot);
}

bool
BWFMSDIO::start(IOService *provider)
{
	uint32_t on = 1;
	struct pie80211_drv *drv;
	int error;

	fPie = NULL;
	fRoot = fNode = NULL;
	if (PE_parse_boot_argn("pie_wifi", &on, sizeof(on)) && on == 0) {
		IOLog("BWFMSDIO: off by pie_wifi=0\n");
		return false;
	}
	if (!PIE80211Controller::start(provider))
		return false;
	fRoot = IORegistryEntry::fromPath("/", gIODTPlane);
	fNode = chipNode(provider);
	if (fNode == NULL)
		IOLog("BWFMSDIO: the chip has no device tree node\n");
	fPie = bwfm_pie_new(reinterpret_cast<struct pie_sdio_func *>(provider), fRoot, fNode);
	drv = fPie != NULL ? bwfm_pie_start(fPie, &error) : NULL;
	if (drv == NULL) {
		IOLog("BWFMSDIO: attach failed: %d\n", fPie != NULL ? error : ENOMEM);
		cleanup();
		PIE80211Controller::stop(provider);
		return false;
	}
	if (!attachDriver(drv)) {
		IOLog("BWFMSDIO: the network interface did not attach\n");
		cleanup();
		PIE80211Controller::stop(provider);
		return false;
	}
	return true;
}

void
BWFMSDIO::stop(IOService *provider)
{
	detachDriver();
	cleanup();
	PIE80211Controller::stop(provider);
}
