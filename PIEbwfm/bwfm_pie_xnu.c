/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * bwfm_pie_os.h on xnu: a mutex for each card, its bus work on the work
 * loop of bwfm_pie_kext.h's worker, and firmware files read with the vnode
 * calls. The kicks have a mutex of their own, so the bus can be kicked
 * under the card's lock. A sleep kicks the bus every 20 ms while it waits,
 * so an answer is read even when the card's interrupt does not come.
 */
#include <mach/mach_types.h>
#include <kern/locks.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <IOKit/IOLib.h>
#include "bwfm_pie_kext.h"
#include "bwfm_pie_os.h"

#define BWFM_PIE_FW_MAX		(1024 * 1024)
#define BWFM_PIE_POLL_NSEC	20000000ULL

void *pie80211_malloc(size_t size, int type, int flags);
void pie80211_free(void *p, int type, size_t size);

/* Each card's locks have a group of their own, freed with them. */
struct bwfm_pie_os {
	lck_grp_t *grp;
	lck_mtx_t *mtx;
	lck_mtx_t *kmtx;
	struct bwfm_pie_worker *worker;
	int stopping;
};

struct bwfm_pie_os *
bwfm_pie_os_alloc(void (*bus_fn)(void *), void *bus_arg)
{
	struct bwfm_pie_os *os;

	os = IOMallocZero(sizeof(*os));
	if (os == NULL)
		return NULL;
	os->grp = lck_grp_alloc_init("bwfm", LCK_GRP_ATTR_NULL);
	if (os->grp != NULL) {
		os->mtx = lck_mtx_alloc_init(os->grp, LCK_ATTR_NULL);
		os->kmtx = lck_mtx_alloc_init(os->grp, LCK_ATTR_NULL);
		os->worker = bwfm_pie_worker_new(bus_fn, bus_arg);
	}
	if (os->grp == NULL || os->mtx == NULL || os->kmtx == NULL || os->worker == NULL) {
		if (os->worker != NULL)
			bwfm_pie_worker_free(os->worker);
		if (os->mtx != NULL)
			lck_mtx_free(os->mtx, os->grp);
		if (os->kmtx != NULL)
			lck_mtx_free(os->kmtx, os->grp);
		if (os->grp != NULL)
			lck_grp_free(os->grp);
		IOFree(os, sizeof(*os));
		return NULL;
	}
	return os;
}

void
bwfm_pie_os_stop(struct bwfm_pie_os *os)
{
	if (os == NULL)
		return;
	lck_mtx_lock(os->kmtx);
	os->stopping = 1;
	lck_mtx_unlock(os->kmtx);
	bwfm_pie_worker_stop(os->worker);
}

void
bwfm_pie_os_free(struct bwfm_pie_os *os)
{
	if (os == NULL)
		return;
	bwfm_pie_os_stop(os);
	bwfm_pie_worker_free(os->worker);
	lck_mtx_free(os->kmtx, os->grp);
	lck_mtx_free(os->mtx, os->grp);
	lck_grp_free(os->grp);
	IOFree(os, sizeof(*os));
}

void
bwfm_pie_os_lock(struct bwfm_pie_os *os)
{
	lck_mtx_lock(os->mtx);
}

void
bwfm_pie_os_unlock(struct bwfm_pie_os *os)
{
	lck_mtx_unlock(os->mtx);
}

int
bwfm_pie_os_sleep(struct bwfm_pie_os *os, void *chan, uint64_t nsec)
{
	uint64_t slept = 0, slice;
	struct timespec ts;

	while (slept < nsec) {
		slice = nsec - slept < BWFM_PIE_POLL_NSEC ? nsec - slept : BWFM_PIE_POLL_NSEC;
		ts.tv_sec = (long)(slice / 1000000000ULL);
		ts.tv_nsec = (long)(slice % 1000000000ULL);
		if (msleep(chan, os->mtx, PZERO, "bwfm", &ts) != EWOULDBLOCK)
			return 0;
		slept += slice;
		bwfm_pie_os_kick(os);
	}
	return 1;
}

void
bwfm_pie_os_wakeup(void *chan)
{
	wakeup(chan);
}

void
bwfm_pie_os_kick(struct bwfm_pie_os *os)
{
	lck_mtx_lock(os->kmtx);
	if (!os->stopping)
		bwfm_pie_worker_kick(os->worker);
	lck_mtx_unlock(os->kmtx);
}

void
bwfm_pie_os_delay(unsigned int us)
{
	IODelay(us);
}

int
bwfm_pie_os_readfile(const char *path, unsigned char **bufp, size_t *lenp)
{
	struct vnode_attr va;
	vfs_context_t ctx;
	vnode_t vp = NULLVP;
	unsigned char *buf = NULL;
	size_t size = 0;
	int error, resid = 0;

	ctx = vfs_context_create(NULL);
	if (ctx == NULL)
		return ENOMEM;
	error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
	if (error != 0)
		goto out;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	error = vnode_getattr(vp, &va, ctx);
	if (error == 0 && (!VATTR_IS_SUPPORTED(&va, va_data_size) || va.va_data_size == 0 ||
	    va.va_data_size > BWFM_PIE_FW_MAX))
		error = EFBIG;
	if (error == 0) {
		size = (size_t)va.va_data_size;
		buf = pie80211_malloc(size, 1, 0);
		if (buf == NULL)
			error = ENOMEM;
	}
	if (error == 0)
		error = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)size, 0, UIO_SYSSPACE, IO_NOAUTH,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	if (error == 0 && resid != 0)
		error = EIO;
	(void)vnode_close(vp, FREAD, ctx);
out:
	vfs_context_rele(ctx);
	if (error != 0) {
		if (buf != NULL)
			pie80211_free(buf, 1, size);
		return error;
	}
	*bufp = buf;
	*lenp = size;
	return 0;
}
