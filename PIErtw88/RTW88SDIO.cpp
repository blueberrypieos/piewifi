/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <pie80211/PIE80211Controller.h>
#include "rtw88_os.h"
#include "rtw88.h"

class RTW88Worker : public OSObject {
	OSDeclareDefaultStructors(RTW88Worker);
	IOWorkLoop *fLoop;
	IOInterruptEventSource *fInterrupt;
	IOTimerEventSource *fTimer;
	void (*fFunction)(void *);
	void *fArgument;
	bool fInterruptAdded;
	bool fTimerAdded;
	bool fStopped;
	static void interrupt(OSObject *owner, IOInterruptEventSource *source, int count);
	static void timer(OSObject *owner, IOTimerEventSource *source);
	static IOReturn stopAction(OSObject *owner, void *, void *, void *, void *);
	void run(void);
public:
	static RTW88Worker *withFunction(void (*function)(void *), void *argument);
	void kick(void);
	void stop(void);
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RTW88Worker, OSObject);

RTW88Worker *
RTW88Worker::withFunction(void (*function)(void *), void *argument)
{
	RTW88Worker *worker = new RTW88Worker;
	if (worker != NULL && !worker->init())
		OSSafeReleaseNULL(worker);
	if (worker == NULL)
		return NULL;
	worker->fLoop = NULL;
	worker->fInterrupt = NULL;
	worker->fTimer = NULL;
	worker->fInterruptAdded = false;
	worker->fTimerAdded = false;
	worker->fStopped = false;
	worker->fFunction = function;
	worker->fArgument = argument;
	worker->fLoop = IOWorkLoop::workLoop();
	if (worker->fLoop != NULL) {
		worker->fInterrupt = IOInterruptEventSource::interruptEventSource(worker, interrupt);
		worker->fTimer = IOTimerEventSource::timerEventSource(worker, timer);
	}
	if (worker->fInterrupt != NULL && worker->fLoop->addEventSource(worker->fInterrupt) == kIOReturnSuccess)
		worker->fInterruptAdded = true;
	if (worker->fTimer != NULL && worker->fLoop->addEventSource(worker->fTimer) == kIOReturnSuccess)
		worker->fTimerAdded = true;
	if (!worker->fTimerAdded || !worker->fInterruptAdded)
		OSSafeReleaseNULL(worker);
	return worker;
}

void
RTW88Worker::run(void)
{
	if (fStopped)
		return;
	fFunction(fArgument);
	if (!fStopped)
		fTimer->setTimeoutMS(100);
}

void
RTW88Worker::interrupt(OSObject *owner, IOInterruptEventSource *source, int count)
{
	(void)source;
	(void)count;
	static_cast<RTW88Worker *>(owner)->run();
}

void
RTW88Worker::timer(OSObject *owner, IOTimerEventSource *source)
{
	(void)source;
	static_cast<RTW88Worker *>(owner)->run();
}

void
RTW88Worker::kick(void)
{
	if (!fStopped)
		fInterrupt->interruptOccurred(NULL, NULL, 0);
}

IOReturn
RTW88Worker::stopAction(OSObject *owner, void *, void *, void *, void *)
{
	RTW88Worker *worker = static_cast<RTW88Worker *>(owner);
	worker->fStopped = true;
	if (worker->fTimerAdded) {
		worker->fTimer->cancelTimeout();
		(void)worker->fLoop->removeEventSource(worker->fTimer);
		worker->fTimerAdded = false;
	}
	if (worker->fInterruptAdded) {
		(void)worker->fLoop->removeEventSource(worker->fInterrupt);
		worker->fInterruptAdded = false;
	}
	return kIOReturnSuccess;
}

void
RTW88Worker::stop(void)
{
	if (fLoop != NULL)
		(void)fLoop->runAction(stopAction, this);
	else
		fStopped = true;
}

void
RTW88Worker::free(void)
{
	stop();
	OSSafeReleaseNULL(fTimer);
	OSSafeReleaseNULL(fInterrupt);
	OSSafeReleaseNULL(fLoop);
	OSObject::free();
}

extern "C" struct rtw88_worker *
rtw88_worker_new(void (*function)(void *), void *argument)
{
	return reinterpret_cast<struct rtw88_worker *>(RTW88Worker::withFunction(function, argument));
}
extern "C" void
rtw88_worker_kick(struct rtw88_worker *worker)
{
	reinterpret_cast<RTW88Worker *>(worker)->kick();
}
extern "C" void
rtw88_worker_stop(struct rtw88_worker *worker)
{
	reinterpret_cast<RTW88Worker *>(worker)->stop();
}
extern "C" void
rtw88_worker_free(struct rtw88_worker *worker)
{
	reinterpret_cast<RTW88Worker *>(worker)->release();
}

class RTW88SDIO : public PIE80211Controller {
	OSDeclareDefaultStructors(RTW88SDIO);
	struct rtw88_pie *fPie;
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RTW88SDIO, PIE80211Controller);

bool
RTW88SDIO::start(IOService *provider)
{
	uint32_t enabled = 1;
	struct pie80211_drv *driver;
	int error = ENOMEM;
	fPie = NULL;
	if (PE_parse_boot_argn("pie_wifi", &enabled, sizeof(enabled)) && enabled == 0)
		return false;
	if (!PIE80211Controller::start(provider))
		return false;
	static const struct rtw88_board_profile initialProfile = {
		0x55,
		{true, 600, 1000},
		{true, 600, 1000},
	};
	fPie = rtw88_pie_new(reinterpret_cast<struct pie_sdio_func *>(provider), &initialProfile);
	driver = fPie != NULL ? rtw88_pie_attach(fPie, &error) : NULL;
	if (driver == NULL || !attachDriver(driver)) {
		IOLog("RTW88SDIO: attach failed: %d\n", error);
		rtw88_pie_delete(fPie);
		fPie = NULL;
		PIE80211Controller::stop(provider);
		return false;
	}
	return true;
}

void
RTW88SDIO::stop(IOService *provider)
{
	detachDriver();
	rtw88_pie_delete(fPie);
	fPie = NULL;
	PIE80211Controller::stop(provider);
}
