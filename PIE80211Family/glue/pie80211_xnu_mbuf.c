/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <pie80211/family_xnu.h>

struct pie80211_out {
	mbuf_t xm;
	size_t at;
};

struct pie80211_mbuf *
pie80211_m_from_xnu(mbuf_t xm, size_t lead)
{
	struct pie80211_mbuf *m;
	void *data;
	size_t len;

	if (xm == NULL || (mbuf_flags(xm) & MBUF_PKTHDR) == 0)
		return NULL;
	len = mbuf_pkthdr_len(xm);
	if (len == 0 || lead > PIE80211_MLEN || len > PIE80211_MAXEXT - lead)
		return NULL;
	m = pie80211_m_alloc(len, lead, &data);
	if (m != NULL && mbuf_copydata(xm, 0, len, data) != 0)
		m = pie80211_m_freem(m);
	return m;
}

static int
pie80211_out_seg(void *arg, const void *data, size_t len)
{
	struct pie80211_out *out = arg;
	errno_t error = mbuf_copyback(out->xm, out->at, len, data, MBUF_DONTWAIT);

	out->at += len;
	return error;
}

int
pie80211_m_to_xnu(struct pie80211_mbuf *m, mbuf_t *xmp)
{
	struct pie80211_out out = { NULL, 0 };
	size_t len = pie80211_m_pktlen(m);
	int error;

	*xmp = NULL;
	if (len == 0 || len > PIE80211_MAXEXT)
		error = EINVAL;
	else if ((error = mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_DATA, &out.xm)) == 0 &&
	    (error = pie80211_m_foreach(m, pie80211_out_seg, &out)) == 0 &&
	    mbuf_pkthdr_len(out.xm) != len)
		error = EINVAL;
	if (error == 0)
		*xmp = out.xm;
	else if (out.xm != NULL)
		mbuf_freem(out.xm);
	pie80211_m_freem(m);
	return error;
}
