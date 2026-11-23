/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include <pie80211/driver.h>
#include <pie80211/fullmac.h>
#include <pie80211/family.h>
#include <pie80211/ioctl.h>
#include "pie80211_ifops.h"

#define DRV_DWELL_MSEC		200
#define DRV_RXQ_MAX		256

struct pie80211_drv {
	struct ieee80211com d_ic;
	struct ieee80211com *d_icp;
	const struct pie80211_drv_ops *d_ops;
	const struct pie80211_fullmac_ops *d_fmops;
	struct timeout d_fm_wd_to;
	void *d_softc;
	struct pie80211_drv_host d_host;
	int (*d_newstate)(struct ieee80211com *, enum ieee80211_state, int);
	int (*d_set_key)(struct ieee80211com *, struct ieee80211_node *, struct ieee80211_key *);
	enum ieee80211_state d_nstate;
	int d_narg;
	int d_state_busy;
	int d_state_again;
	struct mbuf_list d_rxq;
	struct ieee80211_key *d_hwkey;
	struct ieee80211_key *d_keypend;
	int d_key_error;
	int d_start_error;
	uint32_t d_problem;
	uint8_t d_problem_ssid[IEEE80211_NWID_LEN];
	uint8_t d_problem_len;
	uint32_t d_tx_cookie;
	int d_tx_full;
	u_int d_eapol_lost;
	struct timeout d_scan_to;
	struct timeout d_wd_to;
	struct task d_state_task;
	struct task d_start_task;
	struct task d_key_task;
	struct task d_rx_task;
	struct task d_lost_task;
	struct task d_rejoin_task;
	int d_rejoining;
	struct task d_restart_task;
	struct task d_refuse_task;
	struct task d_txst_task;
	struct task d_txwake_task;
};

static void drv_if_input(struct ifnet *, struct mbuf_list *);
static void drv_if_link(struct ifnet *);

static const struct pie80211_ifops drv_ifops = {
	.input = drv_if_input,
	.link_state = drv_if_link,
};

static int
drv_state_of(enum ieee80211_state s)
{
	switch (s) {
	case IEEE80211_S_SCAN:
		return PIE80211_DRV_S_SCAN;
	case IEEE80211_S_AUTH:
		return PIE80211_DRV_S_AUTH;
	case IEEE80211_S_ASSOC:
		return PIE80211_DRV_S_ASSOC;
	case IEEE80211_S_RUN:
		return PIE80211_DRV_S_RUN;
	default:
		return PIE80211_DRV_S_INIT;
	}
}

static void
drv_problem(struct pie80211_drv *d, uint32_t problem, const struct ieee80211_node *ni)
{
	struct ifnet *ifp = &d->d_icp->ic_if;
	static const char *const what[] = {
		[PIE80211_PROBLEM_GROUP_CIPHER] = "the network's group cipher is not supported",
		[PIE80211_PROBLEM_PAIRWISE_CIPHER] = "the network's pairwise cipher is not supported",
		[PIE80211_PROBLEM_KEY_INSTALL] = "the driver could not install the key",
		[PIE80211_PROBLEM_EAPOL_NOACK] = "the access point did not acknowledge a handshake frame",
		[PIE80211_PROBLEM_HANDSHAKE] = "the network ended the WPA2 handshake: the passphrase may be wrong",
	};
	int same;

	same = d->d_problem == problem && ni != NULL && d->d_problem_len == ni->ni_esslen &&
	    memcmp(d->d_problem_ssid, ni->ni_essid, ni->ni_esslen) == 0;
	d->d_problem = problem;
	d->d_problem_len = 0;
	if (ni != NULL && ni->ni_esslen <= IEEE80211_NWID_LEN) {
		memcpy(d->d_problem_ssid, ni->ni_essid, ni->ni_esslen);
		d->d_problem_len = ni->ni_esslen;
	}
	if (same || problem == PIE80211_PROBLEM_NONE || problem >= nitems(what))
		return;
	printf("%s: ", ifp->if_xname);
	if (ni != NULL)
		ieee80211_print_essid(ni->ni_essid, ni->ni_esslen);
	printf(": %s\n", what[problem]);
}

static void
drv_fill_state(struct pie80211_drv *d, struct pie80211_drv_state *st)
{
	struct ieee80211com *ic = &d->d_ic;
	struct ieee80211_node *ni = ic->ic_bss;

	memset(st, 0, sizeof(*st));
	st->state = drv_state_of(d->d_nstate);
	st->prev = drv_state_of(ic->ic_state);
	if (ni == NULL)
		return;
	if (ni->ni_chan != NULL && ni->ni_chan != IEEE80211_CHAN_ANYC)
		st->freq = ni->ni_chan->ic_freq;
	if (d->d_nstate < IEEE80211_S_AUTH)
		return;
	memcpy(st->bssid, ni->ni_bssid, sizeof(st->bssid));
	st->aid = IEEE80211_AID(ni->ni_associd);
	st->beacon_int = ni->ni_intval;
	st->dtim_period = ni->ni_dtimperiod;
	if (ISSET(ic->ic_flags, IEEE80211_F_SHSLOT))
		st->erp |= PIE80211_DRV_ERP_SHORT_SLOT;
	if (ISSET(ic->ic_flags, IEEE80211_F_SHPREAMBLE))
		st->erp |= PIE80211_DRV_ERP_SHORT_PREAMBLE;
	if (ISSET(ic->ic_flags, IEEE80211_F_USEPROT))
		st->erp |= PIE80211_DRV_ERP_PROTECTION;
	st->nrates = (uint8_t)MIN(ni->ni_rates.rs_nrates, PIE80211_DRV_MAXRATES);
	memcpy(st->rates, ni->ni_rates.rs_rates, st->nrates);
}

static void
drv_state_send(struct pie80211_drv *d)
{
	struct pie80211_drv_state st;

	drv_fill_state(d, &st);
	d->d_state_busy = 1;
	d->d_state_again = 0;
	d->d_ops->set_state(d->d_softc, &st);
}

static int
drv_refuse_bss(struct pie80211_drv *d)
{
	struct ieee80211com *ic = d->d_icp;
	struct ieee80211_node *ni = ic->ic_bss, *tree;

	if (!ISSET(ic->ic_flags, IEEE80211_F_RSNON) ||
	    ni->ni_rsngroupcipher == IEEE80211_CIPHER_CCMP)
		return 0;
	drv_problem(d, PIE80211_PROBLEM_GROUP_CIPHER, ni);
	tree = ieee80211_find_node(ic, ni->ni_macaddr);
	if (tree != NULL)
		tree->ni_fails++;
	task_add(systq, &d->d_refuse_task);
	return 1;
}

static void
drv_drop_hwkey(struct pie80211_drv *d)
{
	struct ieee80211_key *k = d->d_hwkey != NULL ? d->d_hwkey : d->d_keypend;
	struct pie80211_drv_key dk;

	if (k == NULL)
		return;
	memset(&dk, 0, sizeof(dk));
	if (d->d_ic.ic_bss != NULL)
		memcpy(dk.addr, d->d_ic.ic_bss->ni_macaddr, sizeof(dk.addr));
	d->d_hwkey = d->d_keypend = NULL;
	d->d_ops->delete_key(d->d_softc, &dk);
	explicit_bzero(k, sizeof(*k));
}

/*
 * A WPA2 association that ends before its port opens, with no other
 * problem seen: most often a wrong passphrase, as the access point drops
 * the station when the handshake's MIC does not check. A rejoin that a
 * change of the stored networks asked for is not the network's doing.
 */
static void
drv_check_handshake(struct pie80211_drv *d, struct ieee80211com *ic, enum ieee80211_state nstate)
{
	struct ieee80211_node *ni = ic->ic_bss;

	if (!d->d_rejoining && ic->ic_state == IEEE80211_S_RUN && nstate != IEEE80211_S_RUN &&
	    nstate != IEEE80211_S_INIT && ISSET(ic->ic_flags, IEEE80211_F_RSNON) && ni != NULL &&
	    !ni->ni_port_valid && d->d_problem == PIE80211_PROBLEM_NONE)
		drv_problem(d, PIE80211_PROBLEM_HANDSHAKE, ni);
}

static int
drv_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct pie80211_drv *d = (struct pie80211_drv *)ic;

	KASSERT(!ISSET(ic->ic_caps, IEEE80211_C_MFP));
	drv_check_handshake(d, ic, nstate);
	timeout_del(&d->d_scan_to);
	if (ic->ic_state == IEEE80211_S_RUN && nstate != IEEE80211_S_RUN)
		drv_drop_hwkey(d);
	if (nstate == IEEE80211_S_SCAN)
		CLR(ic->ic_flags, IEEE80211_F_ASCAN);
	if (nstate == IEEE80211_S_AUTH && ic->ic_state == IEEE80211_S_SCAN && drv_refuse_bss(d))
		return 0;
	d->d_nstate = nstate;
	d->d_narg = arg;
	if (d->d_state_busy) {
		d->d_state_again = 1;
		return 0;
	}
	drv_state_send(d);
	return 0;
}

static void
drv_state_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = &d->d_ic;

	d->d_state_busy = 0;
	if (d->d_state_again) {
		drv_state_send(d);
		return;
	}
	if (!ISSET(ic->ic_if.if_flags, IFF_RUNNING))
		return;
	(void)d->d_newstate(ic, d->d_nstate, d->d_narg);
	if (ic->ic_state == IEEE80211_S_SCAN && !d->d_state_busy)
		timeout_add_msec(&d->d_scan_to, DRV_DWELL_MSEC);
	if (ic->ic_state == IEEE80211_S_RUN && d->d_problem != PIE80211_PROBLEM_NONE &&
	    !ISSET(ic->ic_flags, IEEE80211_F_RSNON))
		drv_problem(d, PIE80211_PROBLEM_NONE, NULL);
}

static void
drv_scan_next(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = &d->d_ic;

	if (ic->ic_state != IEEE80211_S_SCAN || !ISSET(ic->ic_if.if_flags, IFF_RUNNING))
		return;
	if (d->d_state_busy) {
		timeout_add_msec(&d->d_scan_to, DRV_DWELL_MSEC);
		return;
	}
	CLR(ic->ic_flags, IEEE80211_F_ASCAN);
	ieee80211_next_scan(&ic->ic_if);
}

static void
drv_refuse_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = d->d_icp;

	if (ISSET(ic->ic_if.if_flags, IFF_RUNNING) && ic->ic_state == IEEE80211_S_SCAN &&
	    !d->d_state_busy)
		ieee80211_begin_scan(&ic->ic_if);
}

static void
drv_watchdog(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ifnet *ifp = &d->d_ic.ic_if;

	if (!ISSET(ifp->if_flags, IFF_RUNNING))
		return;
	if (ifp->if_timer > 0 && --ifp->if_timer == 0)
		ieee80211_watchdog(ifp);
	timeout_add_sec(&d->d_wd_to, 1);
}

static int
drv_set_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	struct pie80211_drv *d = (struct pie80211_drv *)ic;
	struct pie80211_drv_key dk;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP) {
		drv_problem(d, ISSET(k->k_flags, IEEE80211_KEY_GROUP) ?
		    PIE80211_PROBLEM_GROUP_CIPHER : PIE80211_PROBLEM_PAIRWISE_CIPHER, ni);
		return EOPNOTSUPP;
	}
	if (ISSET(k->k_flags, IEEE80211_KEY_GROUP | IEEE80211_KEY_IGTK) || ni == NULL)
		return ieee80211_set_key(ic, ni, k);
	memset(&dk, 0, sizeof(dk));
	memcpy(dk.addr, ni->ni_macaddr, sizeof(dk.addr));
	dk.len = (uint8_t)MIN(k->k_len, sizeof(dk.key));
	memcpy(dk.key, k->k_key, dk.len);
	dk.rsc = k->k_rsc[0];
	dk.tsc = k->k_tsc;
	d->d_hwkey = NULL;
	d->d_keypend = k;
	d->d_ops->set_key(d->d_softc, &dk);
	explicit_bzero(&dk, sizeof(dk));
	return EBUSY;
}

static void
drv_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	struct pie80211_drv *d = (struct pie80211_drv *)ic;
	struct pie80211_drv_key dk;

	if (k != d->d_hwkey && k != d->d_keypend) {
		ieee80211_delete_key(ic, ni, k);
		return;
	}
	memset(&dk, 0, sizeof(dk));
	if (ni != NULL)
		memcpy(dk.addr, ni->ni_macaddr, sizeof(dk.addr));
	d->d_hwkey = d->d_keypend = NULL;
	d->d_ops->delete_key(d->d_softc, &dk);
	explicit_bzero(k, sizeof(*k));
}

static void
drv_key_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = &d->d_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct ieee80211_key *k = d->d_keypend;

	d->d_keypend = NULL;
	if (k == NULL || ni == NULL || k != &ni->ni_pairwise_key || ic->ic_state != IEEE80211_S_RUN)
		return;
	if (d->d_key_error != 0) {
		drv_problem(d, PIE80211_PROBLEM_KEY_INSTALL, ni);
		IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		return;
	}
	d->d_hwkey = k;
	if (ISSET(ni->ni_flags, IEEE80211_NODE_TXRXPROT) && !ni->ni_port_valid) {
		ni->ni_port_valid = 1;
		ieee80211_set_link_state(ic, LINK_STATE_UP);
		drv_problem(d, PIE80211_PROBLEM_NONE, NULL);
	}
}

static void
drv_updateedca(struct ieee80211com *ic)
{
	struct pie80211_drv *d = (struct pie80211_drv *)ic;
	struct pie80211_drv_edca e[EDCA_NUM_AC];
	int ac;

	for (ac = 0; ac < EDCA_NUM_AC; ac++) {
		e[ac].aifsn = ic->ic_edca_ac[ac].ac_aifsn;
		e[ac].ecwmin = ic->ic_edca_ac[ac].ac_ecwmin;
		e[ac].ecwmax = ic->ic_edca_ac[ac].ac_ecwmax;
		e[ac].acm = ic->ic_edca_ac[ac].ac_acm;
		e[ac].txop = ic->ic_edca_ac[ac].ac_txoplimit;
	}
	d->d_ops->set_edca(d->d_softc, e);
}

static struct mbuf *
drv_ccmp_hdr(struct mbuf *m, struct ieee80211_key *k)
{
	uint8_t *ivp;
	int hdrlen;

	hdrlen = ieee80211_get_hdrlen(mtod(m, struct ieee80211_frame *));
	M_PREPEND(m, IEEE80211_CCMP_HDRLEN, M_DONTWAIT);
	if (m == NULL)
		return NULL;
	if (m->m_len < hdrlen + IEEE80211_CCMP_HDRLEN &&
	    (m = m_pullup(m, hdrlen + IEEE80211_CCMP_HDRLEN)) == NULL)
		return NULL;
	memmove(mtod(m, uint8_t *), mtod(m, uint8_t *) + IEEE80211_CCMP_HDRLEN, hdrlen);
	ivp = mtod(m, uint8_t *) + hdrlen;
	k->k_tsc++;
	ivp[0] = (uint8_t)k->k_tsc;
	ivp[1] = (uint8_t)(k->k_tsc >> 8);
	ivp[2] = 0;
	ivp[3] = (uint8_t)(k->k_id << 6) | IEEE80211_WEP_EXTIV;
	ivp[4] = (uint8_t)(k->k_tsc >> 16);
	ivp[5] = (uint8_t)(k->k_tsc >> 24);
	ivp[6] = (uint8_t)(k->k_tsc >> 32);
	ivp[7] = (uint8_t)(k->k_tsc >> 40);
	return m;
}

static int
drv_is_eapol(struct mbuf *m, int hdrlen)
{
	static const uint8_t snap[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
	uint8_t llc[sizeof(snap)];

	if (m->m_pkthdr.len < hdrlen + (int)sizeof(llc))
		return 0;
	m_copydata(m, hdrlen, sizeof(llc), llc);
	return memcmp(llc, snap, sizeof(llc)) == 0;
}

static int
drv_tx_one(struct pie80211_drv *d, struct mbuf *m, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &d->d_ic;
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	struct pie80211_drv_tx tx;
	struct ieee80211_key *k;
	int tid = 0;

	memset(&tx, 0, sizeof(tx));
	if (IEEE80211_IS_MULTICAST(wh->i_addr1))
		tx.flags |= PIE80211_DRV_TX_NO_ACK;
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT)
		tx.flags |= PIE80211_DRV_TX_MGMT;
	if (ieee80211_has_qos(wh))
		tid = ieee80211_get_qos(wh) & IEEE80211_QOS_TID;
	tx.tid = (uint8_t)tid;
	tx.ac = (uint8_t)ieee80211_up_to_ac(ic, tid);
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    drv_is_eapol(m, ieee80211_get_hdrlen(wh))) {
		if (++d->d_tx_cookie == 0)
			d->d_tx_cookie = 1;
		tx.cookie = d->d_tx_cookie;
		tx.flags |= PIE80211_DRV_TX_STATUS;
	}
	if (ISSET(wh->i_fc[1], IEEE80211_FC1_PROTECTED)) {
		k = ieee80211_get_txkey(ic, wh, ni);
		if (k != NULL && k == d->d_hwkey && k->k_cipher == IEEE80211_CIPHER_CCMP) {
			if ((m = drv_ccmp_hdr(m, k)) == NULL)
				return ENOBUFS;
			tx.flags |= PIE80211_DRV_TX_HW_KEY;
		} else if (k != NULL && ISSET(k->k_flags, IEEE80211_KEY_SWCRYPTO)) {
			if ((m = ieee80211_encrypt(ic, m, k)) == NULL)
				return ENOBUFS;
		} else {
			m_freem(m);
			return EIO;
		}
	}
	return d->d_ops->tx(d->d_softc, m, &tx);
}

static void
drv_start(struct ifnet *ifp)
{
	struct pie80211_drv *d = ifp->if_softc;
	struct ieee80211com *ic = &d->d_ic;
	struct ieee80211_node *ni;
	struct mbuf *m;

	if (!ISSET(ifp->if_flags, IFF_RUNNING))
		return;
	for (;;) {
		m = mq_dequeue(&ic->ic_mgtq);
		if (m != NULL) {
			ni = m->m_pkthdr.ph_cookie;
		} else {
			if (ic->ic_state != IEEE80211_S_RUN ||
			    ISSET(ic->ic_xflags, IEEE80211_F_TX_MGMT_ONLY))
				break;
			m = pie80211_ifq_dequeue(&ifp->if_snd);
			if (m == NULL)
				break;
			m = ieee80211_encap(ifp, m, &ni);
			if (m == NULL) {
				ifp->if_oerrors++;
				continue;
			}
		}
		if (drv_tx_one(d, m, ni) != 0)
			ifp->if_oerrors++;
		if (ni != NULL)
			ieee80211_release_node(ic, ni);
	}
}

static void
drv_rx_one(struct pie80211_drv *d, struct mbuf *m, struct mbuf_list *ml)
{
	struct ieee80211com *ic = &d->d_ic;
	struct ieee80211_frame *wh;
	struct ieee80211_rxinfo rxi;
	struct ieee80211_node *ni;
	struct ieee80211_key *k;
	int rssi, hdrlen;

	if (m->m_pkthdr.len < (int)sizeof(struct ieee80211_frame_min)) {
		ic->ic_stats.is_rx_tooshort++;
		m_freem(m);
		return;
	}
	if ((m = m_pullup(m, MIN(m->m_pkthdr.len, (int)sizeof(struct ieee80211_frame)))) == NULL)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	memset(&rxi, 0, sizeof(rxi));
	rssi = m->m_pkthdr.ph_rssi + PIE80211_RSSI_OFFSET;
	rxi.rxi_rssi = MAX(0, MIN(rssi, PIE80211_RSSI_OFFSET));
	rxi.rxi_tstamp = m->m_pkthdr.ph_tstamp;
	if (m->m_pkthdr.ph_freq != 0)
		rxi.rxi_chan = (uint8_t)ieee80211_mhz2ieee(m->m_pkthdr.ph_freq, 0);
	ni = ieee80211_find_rxnode(ic, wh);
	if (ISSET(m->m_pkthdr.ph_rxflags, PIE80211_DRV_RX_DECRYPTED)) {
		hdrlen = ieee80211_get_hdrlen(wh);
		k = ieee80211_get_rxkey(ic, m, ni);
		if (k == NULL || k != d->d_hwkey || k->k_cipher != IEEE80211_CIPHER_CCMP ||
		    !ISSET(wh->i_fc[1], IEEE80211_FC1_PROTECTED) ||
		    m->m_pkthdr.len < hdrlen + IEEE80211_CCMP_HDRLEN + IEEE80211_CCMP_MICLEN) {
			ic->ic_stats.is_rx_wepfail++;
			m_freem(m);
			ieee80211_release_node(ic, ni);
			return;
		}
		m_adj(m, -IEEE80211_CCMP_MICLEN);
		rxi.rxi_flags |= IEEE80211_RXI_HWDEC;
	}
	ieee80211_inputm(&ic->ic_if, m, ni, &rxi, ml);
	ieee80211_release_node(ic, ni);
}

static void
drv_rx_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m;

	for (;;) {
		pie80211_lock();
		m = pie80211_ml_dequeue(&d->d_rxq);
		pie80211_unlock();
		if (m == NULL)
			break;
		if (!ISSET(d->d_ic.ic_if.if_flags, IFF_RUNNING)) {
			m_freem(m);
			continue;
		}
		drv_rx_one(d, m, &ml);
	}
	if_input(&d->d_ic.ic_if, &ml);
}

static void
drv_if_input(struct ifnet *ifp, struct mbuf_list *ml)
{
	struct pie80211_drv *d = ifp->if_pie_arg;
	struct mbuf *m;

	while ((m = pie80211_ml_dequeue(ml)) != NULL) {
		ifp->if_ipackets++;
		if (d->d_host.input != NULL)
			d->d_host.input(d->d_host.ctx, m);
		else
			m_freem(m);
	}
}

static void
drv_if_link(struct ifnet *ifp)
{
	struct pie80211_drv *d = ifp->if_pie_arg;

	if (ifp->if_link_state == LINK_STATE_UP)
		drv_problem(d, PIE80211_PROBLEM_NONE, NULL);
	if (d->d_host.link != NULL)
		d->d_host.link(d->d_host.ctx, ifp->if_link_state == LINK_STATE_UP,
		    ifp->if_baudrate);
}

static void
drv_start_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = &d->d_ic;
	struct ifnet *ifp = &ic->ic_if;

	if (!ISSET(ifp->if_flags, IFF_UP) || ISSET(ifp->if_flags, IFF_RUNNING))
		return;
	if (d->d_start_error != 0) {
		printf("%s: the driver did not start: error %d\n", ifp->if_xname, d->d_start_error);
		CLR(ifp->if_flags, IFF_UP);
		return;
	}
	SET(ifp->if_flags, IFF_RUNNING);
	timeout_add_sec(&d->d_wd_to, 1);
	ieee80211_begin_scan(ifp);
}

static void
drv_lost_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = &d->d_ic;

	if (ic->ic_state == IEEE80211_S_RUN && ic->ic_mgt_timer == 0)
		IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_PROBE_REQ, 0);
}

static void
drv_rejoin_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = d->d_icp;

	if (ISSET(ic->ic_if.if_flags, IFF_RUNNING) && ic->ic_state != IEEE80211_S_SCAN) {
		d->d_rejoining = 1;
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		d->d_rejoining = 0;
	}
}

static void
drv_txst_task(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ieee80211com *ic = &d->d_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	u_int lost;

	pie80211_lock();
	lost = d->d_eapol_lost;
	d->d_eapol_lost = 0;
	pie80211_unlock();
	if (lost > 0 && ic->ic_state == IEEE80211_S_RUN && ni != NULL && !ni->ni_port_valid)
		drv_problem(d, PIE80211_PROBLEM_EAPOL_NOACK, ni);
}

static void
drv_txwake_task(void *arg)
{
	struct pie80211_drv *d = arg;

	if (d->d_host.tx_wake != NULL)
		d->d_host.tx_wake(d->d_host.ctx);
}

static void
drv_restart_task(void *arg)
{
	struct pie80211_drv *d = arg;

	if (d->d_host.restart != NULL)
		d->d_host.restart(d->d_host.ctx);
}

static int
drv_fm_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct pie80211_drv *d = ic->ic_if.if_pie_arg;

	if (nstate == IEEE80211_S_AUTH && ic->ic_state == IEEE80211_S_SCAN && drv_refuse_bss(d))
		return 0;
	drv_check_handshake(d, ic, nstate);
	return d->d_newstate(ic, nstate, arg);
}

static int
drv_fm_set_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	struct pie80211_drv *d = ic->ic_if.if_pie_arg;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP) {
		drv_problem(d, ISSET(k->k_flags, IEEE80211_KEY_GROUP) ?
		    PIE80211_PROBLEM_GROUP_CIPHER : PIE80211_PROBLEM_PAIRWISE_CIPHER, ni);
		return EOPNOTSUPP;
	}
	return d->d_set_key(ic, ni, k);
}

static void
drv_fm_watchdog(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ifnet *ifp = &d->d_icp->ic_if;

	if (!ISSET(ifp->if_flags, IFF_UP))
		return;
	if (ifp->if_timer > 0 && --ifp->if_timer == 0 && ifp->if_watchdog != NULL)
		ifp->if_watchdog(ifp);
	timeout_add_sec(&d->d_fm_wd_to, 1);
}

static int
drv_fm_up(struct pie80211_drv *d)
{
	struct ifnet *ifp = &d->d_icp->ic_if;

	if (ISSET(ifp->if_flags, IFF_UP))
		return 0;
	SET(ifp->if_flags, IFF_UP);
	d->d_fmops->init(d->d_softc);
	if (!ISSET(ifp->if_flags, IFF_RUNNING)) {
		CLR(ifp->if_flags, IFF_UP);
		return EIO;
	}
	timeout_add_sec(&d->d_fm_wd_to, 1);
	return 0;
}

static void
drv_fm_down(struct pie80211_drv *d)
{
	struct ifnet *ifp = &d->d_icp->ic_if;

	if (!ISSET(ifp->if_flags, IFF_UP))
		return;
	CLR(ifp->if_flags, IFF_UP);
	timeout_del(&d->d_fm_wd_to);
	d->d_fmops->stop(d->d_softc);
	pie80211_ml_purge(&ifp->if_snd.q_pkts);
}

static void
drv_fm_detach(struct pie80211_drv *d)
{
	struct ifnet *ifp = &d->d_icp->ic_if;

	drv_fm_down(d);
	task_del(systq, &d->d_rejoin_task);
	task_del(systq, &d->d_restart_task);
	task_del(systq, &d->d_refuse_task);
	d->d_icp->ic_newstate = d->d_newstate;
	d->d_icp->ic_set_key = d->d_set_key;
	ifp->if_pie = NULL;
	ifp->if_pie_arg = NULL;
	free(d, M_DEVBUF, sizeof(*d));
}

struct drv_attach_ic_args {
	struct ieee80211com *ic;
	const struct pie80211_fullmac_ops *ops;
	void *softc;
	struct pie80211_drv *d;
	int error;
};

static void
drv_attach_ic_gated(void *arg)
{
	struct drv_attach_ic_args *a = arg;
	struct pie80211_drv *d;
	struct ifnet *ifp = &a->ic->ic_if;

	d = malloc(sizeof(*d), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (d == NULL) {
		a->error = ENOMEM;
		return;
	}
	d->d_icp = a->ic;
	d->d_fmops = a->ops;
	d->d_softc = a->softc;
	timeout_set(&d->d_fm_wd_to, drv_fm_watchdog, d);
	task_set(&d->d_rejoin_task, drv_rejoin_task, d);
	task_set(&d->d_restart_task, drv_restart_task, d);
	task_set(&d->d_refuse_task, drv_refuse_task, d);
	ifp->if_pie = &drv_ifops;
	ifp->if_pie_arg = d;
	d->d_newstate = a->ic->ic_newstate;
	a->ic->ic_newstate = drv_fm_newstate;
	d->d_set_key = a->ic->ic_set_key;
	a->ic->ic_set_key = drv_fm_set_key;
	a->d = d;
	a->error = 0;
}

struct pie80211_drv *
pie80211_drv_attach_ic(struct ieee80211com *ic, const struct pie80211_fullmac_ops *ops, void *softc,
    int *error)
{
	struct drv_attach_ic_args a = { ic, ops, softc, NULL, EINVAL };

	if (ops == NULL || ops->init == NULL || ops->stop == NULL) {
		*error = EINVAL;
		return NULL;
	}
	pie80211_gated(drv_attach_ic_gated, &a);
	*error = a.error;
	return a.d;
}

struct drv_attach_args {
	const struct pie80211_drv_desc *desc;
	const struct pie80211_drv_ops *ops;
	void *softc;
	struct pie80211_drv *d;
	int error;
};

static void
drv_attach_gated(void *arg)
{
	struct drv_attach_args *a = arg;
	const struct pie80211_drv_desc *desc = a->desc;
	struct pie80211_drv *d;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	const struct pie80211_drv_chan *c;
	u_int i, chan, has5 = 0;

	a->error = EINVAL;
	if (ISSET(desc->caps, PIE80211_DRV_CAP_MFP) || desc->nchan == 0 ||
	    desc->nchan > PIE80211_DRV_MAXCHAN)
		return;
	d = malloc(sizeof(*d), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (d == NULL) {
		a->error = ENOMEM;
		return;
	}
	ic = &d->d_ic;
	d->d_icp = ic;
	ifp = &ic->ic_if;
	strlcpy(ifp->if_xname, "wlan0", sizeof(ifp->if_xname));
	ifp->if_softc = d;
	ifp->if_start = drv_start;
	ifp->if_pie = &drv_ifops;
	ifp->if_pie_arg = d;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_RSN | IEEE80211_C_QOS;
	if (ISSET(desc->caps, PIE80211_DRV_CAP_SHORT_PREAMBLE))
		SET(ic->ic_caps, IEEE80211_C_SHPREAMBLE);
	if (ISSET(desc->caps, PIE80211_DRV_CAP_SHORT_SLOT))
		SET(ic->ic_caps, IEEE80211_C_SHSLOT);
	ic->ic_max_rssi = PIE80211_RSSI_OFFSET;
	memcpy(ic->ic_myaddr, desc->addr, sizeof(ic->ic_myaddr));
	for (i = 0; i < desc->nchan; i++) {
		c = &desc->chan[i];
		chan = ieee80211_mhz2ieee(c->freq, ISSET(c->flags, PIE80211_DRV_CHAN_5GHZ) ?
		    IEEE80211_CHAN_5GHZ : IEEE80211_CHAN_2GHZ);
		if (chan == 0 || chan > IEEE80211_CHAN_MAX)
			continue;
		ic->ic_channels[chan].ic_freq = c->freq;
		if (ISSET(c->flags, PIE80211_DRV_CHAN_5GHZ)) {
			ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_A;
			has5 = 1;
		} else {
			ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_B | IEEE80211_CHAN_G;
		}
		if (ISSET(c->flags, PIE80211_DRV_CHAN_PASSIVE))
			SET(ic->ic_channels[chan].ic_flags, IEEE80211_CHAN_PASSIVE);
	}
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	if (has5)
		ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
	if_attach(ifp);
	ieee80211_ifattach(ifp);
	d->d_newstate = ic->ic_newstate;
	ic->ic_newstate = drv_newstate;
	ic->ic_set_key = drv_set_key;
	ic->ic_delete_key = drv_delete_key;
	ic->ic_updateedca = drv_updateedca;
	ieee80211_media_init(ifp, ieee80211_media_change, ieee80211_media_status);
	d->d_ops = a->ops;
	d->d_softc = a->softc;
	timeout_set(&d->d_scan_to, drv_scan_next, d);
	timeout_set(&d->d_wd_to, drv_watchdog, d);
	task_set(&d->d_state_task, drv_state_task, d);
	task_set(&d->d_start_task, drv_start_task, d);
	task_set(&d->d_key_task, drv_key_task, d);
	task_set(&d->d_rx_task, drv_rx_task, d);
	task_set(&d->d_lost_task, drv_lost_task, d);
	task_set(&d->d_rejoin_task, drv_rejoin_task, d);
	task_set(&d->d_restart_task, drv_restart_task, d);
	task_set(&d->d_refuse_task, drv_refuse_task, d);
	task_set(&d->d_txst_task, drv_txst_task, d);
	task_set(&d->d_txwake_task, drv_txwake_task, d);
	a->d = d;
	a->error = 0;
}

struct pie80211_drv *
pie80211_drv_attach(const struct pie80211_drv_desc *desc, const struct pie80211_drv_ops *ops,
    void *softc, int *error)
{
	struct drv_attach_args a = { desc, ops, softc, NULL, EINVAL };

	pie80211_gated(drv_attach_gated, &a);
	*error = a.error;
	return a.d;
}

static void
drv_detach_gated(void *arg)
{
	struct pie80211_drv *d = arg;
	struct ifnet *ifp = &d->d_ic.ic_if;

	if (d->d_fmops != NULL) {
		drv_fm_detach(d);
		return;
	}
	pie80211_drv_down(d);
	task_del(systq, &d->d_state_task);
	task_del(systq, &d->d_start_task);
	task_del(systq, &d->d_key_task);
	task_del(systq, &d->d_rx_task);
	task_del(systq, &d->d_lost_task);
	task_del(systq, &d->d_rejoin_task);
	task_del(systq, &d->d_restart_task);
	task_del(systq, &d->d_refuse_task);
	task_del(systq, &d->d_txst_task);
	task_del(systq, &d->d_txwake_task);
	ieee80211_ifdetach(ifp);
	if_detach(ifp);
	free(d, M_DEVBUF, sizeof(*d));
}

void
pie80211_drv_detach(struct pie80211_drv *d)
{
	pie80211_gated(drv_detach_gated, d);
}

const uint8_t *
pie80211_drv_addr(struct pie80211_drv *d)
{
	return d->d_icp->ic_myaddr;
}

void
pie80211_drv_set_host(struct pie80211_drv *d, const struct pie80211_drv_host *host)
{
	d->d_host = *host;
}

int
pie80211_drv_up(struct pie80211_drv *d)
{
	struct ifnet *ifp = &d->d_ic.ic_if;

	if (d->d_fmops != NULL)
		return drv_fm_up(d);
	if (ISSET(ifp->if_flags, IFF_UP))
		return 0;
	SET(ifp->if_flags, IFF_UP);
	d->d_start_error = 0;
	d->d_ops->start(d->d_softc);
	return 0;
}

void
pie80211_drv_down(struct pie80211_drv *d)
{
	struct ieee80211com *ic = &d->d_ic;
	struct ifnet *ifp = &ic->ic_if;
	int running = ISSET(ifp->if_flags, IFF_RUNNING);

	if (d->d_fmops != NULL) {
		drv_fm_down(d);
		return;
	}
	if (!ISSET(ifp->if_flags, IFF_UP))
		return;
	CLR(ifp->if_flags, IFF_UP | IFF_RUNNING);
	timeout_del(&d->d_scan_to);
	timeout_del(&d->d_wd_to);
	d->d_state_busy = d->d_state_again = 0;
	drv_drop_hwkey(d);
	if (running && ic->ic_state != IEEE80211_S_INIT)
		(void)d->d_newstate(ic, IEEE80211_S_INIT, -1);
	pie80211_lock();
	pie80211_ml_purge(&d->d_rxq);
	pie80211_unlock();
	pie80211_ml_purge(&ifp->if_snd.q_pkts);
	d->d_ops->stop(d->d_softc);
}

int
pie80211_drv_output(struct pie80211_drv *d, struct pie80211_mbuf *m)
{
	struct ieee80211com *ic = d->d_icp;

	if (!ISSET(ic->ic_if.if_flags, IFF_RUNNING) || ic->ic_state != IEEE80211_S_RUN) {
		m_freem(m);
		return ENETDOWN;
	}
	return if_enqueue(&ic->ic_if, m);
}

static const struct {
	uint32_t cmd;
	uint32_t len;
	int set;
	int root;
} drv_cmds[] = {
	{ PIE80211_CMD_JOIN_ADD, sizeof(struct pie80211_join_req), 1, 1 },
	{ PIE80211_CMD_JOIN_DEL, sizeof(struct pie80211_join_req), 1, 1 },
	{ PIE80211_CMD_JOIN_FLUSH, 0, 1, 1 },
	{ PIE80211_CMD_JOIN_LIST, sizeof(struct pie80211_join_list), 0, 1 },
	{ PIE80211_CMD_SCAN, 0, 1, 1 },
	{ PIE80211_CMD_NODES, sizeof(struct pie80211_nodes), 0, 0 },
	{ PIE80211_CMD_STATUS, sizeof(struct pie80211_status), 0, 0 },
	{ PIE80211_CMD_COUNTRY, sizeof(struct pie80211_country), 1, 1 },
};

int
pie80211_drv_cmdinfo(uint32_t cmd, size_t *len, int *set, int *root)
{
	size_t i;

	for (i = 0; i < nitems(drv_cmds); i++) {
		if (drv_cmds[i].cmd == cmd) {
			*len = drv_cmds[i].len;
			*set = drv_cmds[i].set;
			*root = drv_cmds[i].root;
			return 0;
		}
	}
	return EOPNOTSUPP;
}

int
pie80211_drv_ioctl(struct pie80211_drv *d, uint32_t cmd, void *data, size_t len)
{
	struct pie80211_status *st;
	const struct pie80211_country *c;
	uint32_t rate;
	int8_t rssi;
	int error;

	if (cmd == PIE80211_CMD_COUNTRY) {
		if (len != sizeof(*c))
			return EINVAL;
		c = data;
		if (d->d_fmops != NULL)
			return d->d_fmops->set_country != NULL ?
			    d->d_fmops->set_country(d->d_softc, c->code) : EOPNOTSUPP;
		return d->d_ops->set_country(d->d_softc, c->code);
	}
	error = pie80211_ioctl((struct pie80211_com *)d->d_icp, cmd, data, len);
	if (error == ENETRESET) {
		task_add(systq, &d->d_rejoin_task);
		error = 0;
	}
	if (error == 0 && cmd == PIE80211_CMD_STATUS) {
		st = data;
		if (d->d_fmops != NULL && d->d_fmops->link_info != NULL &&
		    st->state == PIE80211_STATE_RUN &&
		    d->d_fmops->link_info(d->d_softc, &rssi, &rate) == 0) {
			st->rssi = rssi;
			st->rate_kbps = rate;
		}
		st->problem = d->d_problem;
		st->problem_ssid.len = d->d_problem_len;
		memcpy(st->problem_ssid.bytes, d->d_problem_ssid, d->d_problem_len);
	}
	return error;
}

void
pie80211_drv_started(struct pie80211_drv *d, int error)
{
	pie80211_lock();
	d->d_start_error = error;
	pie80211_unlock();
	task_add(systq, &d->d_start_task);
}

void
pie80211_drv_state_done(struct pie80211_drv *d)
{
	task_add(systq, &d->d_state_task);
}

void
pie80211_drv_key_done(struct pie80211_drv *d, int error)
{
	pie80211_lock();
	d->d_key_error = error;
	pie80211_unlock();
	task_add(systq, &d->d_key_task);
}

void
pie80211_drv_input(struct pie80211_drv *d, struct pie80211_mbuf *m, const struct pie80211_drv_rx *rx)
{
	int queued = 0;

	m->m_pkthdr.ph_rxflags = rx->flags;
	m->m_pkthdr.ph_freq = rx->freq;
	m->m_pkthdr.ph_rssi = rx->rssi;
	m->m_pkthdr.ph_tstamp = rx->tstamp;
	pie80211_lock();
	if (d->d_rxq.mh_count < DRV_RXQ_MAX) {
		pie80211_ml_enqueue(&d->d_rxq, m);
		queued = 1;
	}
	pie80211_unlock();
	if (queued)
		task_add(systq, &d->d_rx_task);
	else
		m_freem(m);
}

void
pie80211_drv_tx_done(struct pie80211_drv *d, uint32_t cookie, int acked)
{
	if (acked)
		return;
	pie80211_lock();
	d->d_eapol_lost++;
	pie80211_unlock();
	task_add(systq, &d->d_txst_task);
}

void
pie80211_drv_tx_flow(struct pie80211_drv *d, int full)
{
	int wake;

	pie80211_lock();
	wake = d->d_tx_full && !full;
	d->d_tx_full = full != 0;
	pie80211_unlock();
	if (wake)
		task_add(systq, &d->d_txwake_task);
}

int
pie80211_drv_tx_stopped(struct pie80211_drv *d)
{
	int full;

	pie80211_lock();
	full = d->d_tx_full;
	pie80211_unlock();
	return full;
}

void
pie80211_drv_lost(struct pie80211_drv *d)
{
	task_add(systq, &d->d_lost_task);
}

void
pie80211_drv_restart(struct pie80211_drv *d)
{
	task_add(systq, &d->d_restart_task);
}

void
pie80211_drv_problem(struct pie80211_drv *d, uint32_t problem)
{
	drv_problem(d, problem, problem != PIE80211_PROBLEM_NONE ? d->d_icp->ic_bss : NULL);
}
