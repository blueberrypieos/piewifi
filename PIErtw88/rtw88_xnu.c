/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <stdbool.h>
#include <string.h>
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <IOKit/IOLib.h>
#include <pie80211/names.h>
#ifndef __bounded__
#define __bounded__(...)
#endif
#include <sys/crypto/sha2.h>
#include "rtw88_os.h"

struct rtw88_os {
	lck_grp_t *group;
	lck_mtx_t *mutex;
	lck_mtx_t *kick_mutex;
	struct rtw88_worker *worker;
	bool stopping;
};

void *
rtw88_os_alloc_mem(size_t size)
{
	return IOMallocZero(size);
}

void
rtw88_os_free_mem(void *p, size_t size)
{
	if (p != NULL)
		IOFree(p, size);
}

struct rtw88_os *
rtw88_os_new(void (*fn)(void *), void *arg)
{
	struct rtw88_os *os = IOMallocZero(sizeof(*os));
	if (os == NULL)
		return NULL;
	os->group = lck_grp_alloc_init("rtw88", LCK_GRP_ATTR_NULL);
	if (os->group != NULL) {
		os->mutex = lck_mtx_alloc_init(os->group, LCK_ATTR_NULL);
		os->kick_mutex = lck_mtx_alloc_init(os->group, LCK_ATTR_NULL);
		os->worker = rtw88_worker_new(fn, arg);
	}
	if (os->group == NULL || os->mutex == NULL || os->kick_mutex == NULL || os->worker == NULL) {
		if (os->worker != NULL)
			rtw88_worker_free(os->worker);
		if (os->mutex != NULL)
			lck_mtx_free(os->mutex, os->group);
		if (os->kick_mutex != NULL)
			lck_mtx_free(os->kick_mutex, os->group);
		if (os->group != NULL)
			lck_grp_free(os->group);
		IOFree(os, sizeof(*os));
		return NULL;
	}
	return os;
}

void
rtw88_os_stop(struct rtw88_os *os)
{
	lck_mtx_lock(os->kick_mutex);
	os->stopping = true;
	lck_mtx_unlock(os->kick_mutex);
	rtw88_worker_stop(os->worker);
}

void
rtw88_os_free(struct rtw88_os *os)
{
	rtw88_os_stop(os);
	rtw88_worker_free(os->worker);
	lck_mtx_free(os->kick_mutex, os->group);
	lck_mtx_free(os->mutex, os->group);
	lck_grp_free(os->group);
	IOFree(os, sizeof(*os));
}

void
rtw88_os_lock(struct rtw88_os *os)
{
	lck_mtx_lock(os->mutex);
}

void
rtw88_os_unlock(struct rtw88_os *os)
{
	lck_mtx_unlock(os->mutex);
}

void
rtw88_os_kick(struct rtw88_os *os)
{
	lck_mtx_lock(os->kick_mutex);
	if (!os->stopping)
		rtw88_worker_kick(os->worker);
	lck_mtx_unlock(os->kick_mutex);
}

void
rtw88_os_delay(unsigned int usec)
{
	if (usec >= 1000)
		IOSleep((usec + 999) / 1000);
	else
		IODelay(usec);
}

uint64_t
rtw88_os_now(void)
{
	uint64_t now, nanoseconds;
	clock_get_uptime(&now);
	absolutetime_to_nanoseconds(now, &nanoseconds);
	return nanoseconds;
}

int
rtw88_os_firmware(uint8_t **data, size_t *size)
{
	static const uint8_t expected[32] = {
		0x2e, 0xf4, 0x09, 0xbc, 0x41, 0x85, 0x49, 0xfc,
		0xf2, 0x94, 0x06, 0x1d, 0xd0, 0xca, 0xe1, 0xfc,
		0x22, 0xfd, 0x9d, 0xa7, 0x9b, 0x60, 0x52, 0x49,
		0x50, 0xb2, 0x5d, 0xe1, 0x87, 0x32, 0xf3, 0xf0
	};
	const size_t expected_size = 139472;
	struct vnode_attr attributes;
	vfs_context_t context;
	vnode_t vnode = NULLVP;
	SHA2_CTX hash;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	uint8_t *buffer = NULL;
	int error, residual = 0;
	*data = NULL;
	*size = 0;
	context = vfs_context_create(NULL);
	if (context == NULL)
		return ENOMEM;
	error = vnode_open("/usr/share/firmware/rtw88/rtw8821c_fw.bin", FREAD, 0, 0, &vnode, context);
	if (error != 0)
		goto out;
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	error = vnode_getattr(vnode, &attributes, context);
	if (error == 0 && (!VATTR_IS_SUPPORTED(&attributes, va_data_size) || attributes.va_data_size != expected_size))
		error = EINVAL;
	if (error == 0) {
		buffer = IOMalloc(expected_size);
		if (buffer == NULL)
			error = ENOMEM;
	}
	if (error == 0)
		error = vn_rdwr(UIO_READ, vnode, (caddr_t)buffer, (int)expected_size, 0,
		    UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(context), &residual, vfs_context_proc(context));
	if (error == 0 && residual != 0)
		error = EIO;
	if (error == 0) {
		SHA256Init(&hash);
		SHA256Update(&hash, buffer, expected_size);
		SHA256Final(digest, &hash);
		if (memcmp(digest, expected, sizeof(expected)) != 0)
			error = EACCES;
	}
	(void)vnode_close(vnode, FREAD, context);
out:
	vfs_context_rele(context);
	if (error != 0) {
		if (buffer != NULL)
			IOFree(buffer, expected_size);
		return error;
	}
	*data = buffer;
	*size = expected_size;
	return 0;
}

void
rtw88_os_failure(const char *stage, uint32_t address, int error)
{
	IOLog("rtw88: %s failed at 0x%08x: %d\n", stage, address, error);
}

void
rtw88_os_error(const char *stage, uint32_t address, int error, int retry)
{
	if (address != 0)
		IOLog("RTW88SDIO: %s failed at 0x%08x: %d, recovery %d of 3\n", stage, address, error, retry);
	else
		IOLog("RTW88SDIO: %s failed: %d, recovery %d of 3\n", stage, error, retry);
}
