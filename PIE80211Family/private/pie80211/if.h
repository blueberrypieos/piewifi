/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef _PIE80211_IF_H
#define _PIE80211_IF_H

#include <pie80211/base.h>
#include <pie80211/kern.h>
#include <pie80211/mbuf.h>

#ifndef INET6
#define INET6			1
#endif

#define ifnet			pie80211_ifnet
#define ifqueue			pie80211_ifqueue
#define arpcom			pie80211_arpcom
#define ifmedia			pie80211_ifmedia
#define ifmedia_entry		pie80211_ifmedia_entry
#define ifmediareq		pie80211_ifmediareq
#define if_ieee80211_data	pie80211_if_ieee80211_data
#define ifm_change_cb_t		pie80211_ifm_change_t
#define ifm_stat_cb_t		pie80211_ifm_stat_t
#define etherbroadcastaddr	pie80211_etherbroadcastaddr

struct sockaddr;
struct rtentry;
struct ifnet;
struct ifmediareq;

#ifndef IFNAMSIZ
#define IFNAMSIZ		16
#endif
#define IFQ_MAXLEN		256
#define IFF_UP			0x01
#define IFF_RUNNING		0x02
#define IFF_DEBUG		0x04
#define IFF_SIMPLEX		0x08
#define IFCAP_VLAN_MTU		0x01
#define IF_WIRELESS_DEFAULT_PRIORITY 4
#define LINK_STATE_UNKNOWN	0
#define LINK_STATE_DOWN		2
#define LINK_STATE_UP		4
#define LINK_STATE_IS_UP(s)	((s) == LINK_STATE_UNKNOWN || (s) >= LINK_STATE_UP)
#define IF_Mbps(n)		((uint64_t)(n) * 1000000ULL)

struct ifqueue {
	struct mbuf_list q_pkts;
	u_int q_limit;
	u_int q_drops;
	int q_oactive;
};

struct ifnet {
	char if_xname[IFNAMSIZ];
	void *if_softc;
	const void *if_pie;
	void *if_pie_arg;
	int if_flags;
	int if_timer;
	uint8_t if_link_state;
	uint8_t if_hdrlen;
	uint8_t if_priority;
	uint32_t if_capabilities;
	uint64_t if_baudrate;
	uint64_t if_ipackets;
	uint64_t if_ibytes;
	uint64_t if_imcasts;
	uint64_t if_ierrors;
	uint64_t if_oerrors;
	struct ifqueue if_snd;
	void (*if_start)(struct ifnet *);
	int (*if_output)(struct ifnet *, struct mbuf *, struct sockaddr *, struct rtentry *);
	void (*if_watchdog)(struct ifnet *);
};

#define ETHER_ADDR_LEN		6
#define ETHER_HDR_LEN		14
#define ETHER_ALIGN		2
#define ETHERTYPE_IP		0x0800
#define ETHERTYPE_IPV6		0x86dd
#define ETHERTYPE_EAPOL		0x888e
#define ETHER_IS_MULTICAST(a)	((((const uint8_t *)(a))[0] & 1) != 0)
#define ETHER_IS_EQ(a, b)	(memcmp((a), (b), ETHER_ADDR_LEN) == 0)

struct ether_header {
	uint8_t ether_dhost[ETHER_ADDR_LEN];
	uint8_t ether_shost[ETHER_ADDR_LEN];
	uint16_t ether_type;
} __packed;

extern const uint8_t etherbroadcastaddr[ETHER_ADDR_LEN];

struct arpcom {
	struct ifnet ac_if;
	uint8_t ac_enaddr[ETHER_ADDR_LEN];
};

#define LLC_UI			0x03
#define LLC_SNAP_LSAP		0xaa
#define LLC_SNAPFRAMELEN	8

struct pie80211_snap {
	uint8_t control;
	uint8_t org_code[3];
	uint16_t ether_type;
} __packed;

struct llc {
	uint8_t llc_dsap;
	uint8_t llc_ssap;
	struct pie80211_snap llc_snap;
} __packed;
#define llc_control		llc_snap.control

struct ip {
	uint8_t ip_hl:4;
	uint8_t ip_v:4;
	uint8_t ip_tos;
	uint8_t ip_rest[18];
} __packed;

struct ip6_hdr {
	uint32_t ip6_flow;
	uint8_t ip6_rest[36];
} __packed;

#define PIE80211_DSCP(cp)	((cp) << 2)
#define IPTOS_DSCP_CS1		PIE80211_DSCP(8)
#define IPTOS_DSCP_CS3		PIE80211_DSCP(24)
#define IPTOS_DSCP_AF31		PIE80211_DSCP(26)
#define IPTOS_DSCP_AF32		PIE80211_DSCP(28)
#define IPTOS_DSCP_AF33		PIE80211_DSCP(30)
#define IPTOS_DSCP_CS4		PIE80211_DSCP(32)
#define IPTOS_DSCP_AF41		PIE80211_DSCP(34)
#define IPTOS_DSCP_AF42		PIE80211_DSCP(36)
#define IPTOS_DSCP_AF43		PIE80211_DSCP(38)
#define IPTOS_DSCP_CS5		PIE80211_DSCP(40)
#define IPTOS_DSCP_VA		PIE80211_DSCP(44)
#define IPTOS_DSCP_EF		PIE80211_DSCP(46)
#define IPTOS_DSCP_CS6		PIE80211_DSCP(48)
#define IPTOS_DSCP_CS7		PIE80211_DSCP(56)

#define NBPFILTER		0
#define DLT_IEEE802_11		105
#define DLT_IEEE802_11_RADIO	127

#define IFM_SUBTYPE(w)		((uint64_t)(w) & 0xffffULL)
#define IFM_TYPE(w)		((uint64_t)(w) & 0xff0000ULL)
#define IFM_MODE(w)		((uint64_t)(w) & 0xff000000ULL)
#define IFM_INST(w)		(((uint64_t)(w) >> 48) & 0xffULL)
#define IFM_INST_ANY		UINT64_MAX
#define IFM_MAKEWORD(type, sub, opt, inst) \
	((uint64_t)(type) | (uint64_t)(sub) | (uint64_t)(opt) | ((uint64_t)(inst) << 48))
#define IFM_IEEE80211		(0x80ULL << 16)
#define IFM_IEEE80211_11A	(1ULL << 24)
#define IFM_IEEE80211_11B	(2ULL << 24)
#define IFM_IEEE80211_11G	(3ULL << 24)
#define IFM_IEEE80211_11N	(4ULL << 24)
#define IFM_IEEE80211_11AC	(5ULL << 24)
#define IFM_IEEE80211_11AX	(6ULL << 24)
#define IFM_IEEE80211_ADHOC	(1ULL << 32)
#define IFM_IEEE80211_HOSTAP	(1ULL << 33)
#define IFM_IEEE80211_IBSS	(1ULL << 34)
#define IFM_IEEE80211_MONITOR	(1ULL << 35)
#define IFM_AVALID		1
#define IFM_ACTIVE		2

enum {
	IFM_AUTO,
	IFM_MANUAL,
	IFM_NONE,
	IFM_IEEE80211_DS1,
	IFM_IEEE80211_DS2,
	IFM_IEEE80211_DS5,
	IFM_IEEE80211_DS11,
	IFM_IEEE80211_DS22,
	IFM_IEEE80211_OFDM6,
	IFM_IEEE80211_OFDM9,
	IFM_IEEE80211_OFDM12,
	IFM_IEEE80211_OFDM18,
	IFM_IEEE80211_OFDM24,
	IFM_IEEE80211_OFDM36,
	IFM_IEEE80211_OFDM48,
	IFM_IEEE80211_OFDM54,
	IFM_IEEE80211_OFDM72,
	IFM_IEEE80211_HT_MCS0 = 32,
	IFM_IEEE80211_HT_MCS76 = IFM_IEEE80211_HT_MCS0 + 76,
	IFM_IEEE80211_VHT_MCS0 = 128,
	IFM_IEEE80211_VHT_MCS9 = IFM_IEEE80211_VHT_MCS0 + 9,
	IFM_IEEE80211_HE_MCS0 = 160
};

typedef int (*pie80211_ifm_change_t)(struct ifnet *);
typedef void (*pie80211_ifm_stat_t)(struct ifnet *, struct ifmediareq *);

struct ifmedia_entry {
	TAILQ_ENTRY(ifmedia_entry) me_link;
	uint64_t ifm_media;
};

struct ifmedia {
	TAILQ_HEAD(, ifmedia_entry) m_words;
	struct ifmedia_entry *ifm_cur;
	uint64_t ifm_media;
	pie80211_ifm_change_t m_change;
	pie80211_ifm_stat_t m_status;
};

struct ifmediareq {
	uint64_t ifm_status;
	uint64_t ifm_active;
};

#define ifmedia_init(ifm, mask, ch, st)	pie80211_ifmedia_init(ifm, ch, st)
#define ifmedia_add(ifm, w, data, aux)	pie80211_ifmedia_add(ifm, w)
#define ifmedia_set(ifm, w)		pie80211_ifmedia_set(ifm, w)
#define ifmedia_delete_instance(ifm, i)	pie80211_ifmedia_delete_instance(ifm, i)
#define ifmedia_baudrate(w)		pie80211_ifmedia_baudrate(w)
void pie80211_ifmedia_init(struct ifmedia *ifm, pie80211_ifm_change_t ch,
    pie80211_ifm_stat_t st);
void pie80211_ifmedia_add(struct ifmedia *ifm, uint64_t w);
void pie80211_ifmedia_set(struct ifmedia *ifm, uint64_t w);
void pie80211_ifmedia_delete_instance(struct ifmedia *ifm, uint64_t inst);
uint64_t pie80211_ifmedia_baudrate(uint64_t w);

struct if_ieee80211_data {
	uint32_t ifie_flags;
	uint32_t ifie_xflags;
	uint8_t ifie_addr[ETHER_ADDR_LEN];
	uint8_t ifie_channel;
	uint8_t ifie_nwid_len;
	uint8_t ifie_nwid[32];
};
#define rtm_80211info(ifp, ifie)	((void)(ifp), (void)(ifie))

#define ifq_enqueue(ifq, m)		pie80211_ifq_enqueue(ifq, m)
int pie80211_ifq_enqueue(struct ifqueue *ifq, struct mbuf *m);
struct mbuf *pie80211_ifq_dequeue(struct ifqueue *ifq);

#define if_attach(ifp)			pie80211_if_attach(ifp)
#define if_detach(ifp)			pie80211_if_detach(ifp)
#define if_deactivate(ifp)		((void)(ifp))
#define if_addgroup(ifp, g)		((void)(ifp), 0)
#define if_input(ifp, ml)		pie80211_if_input(ifp, ml)
#define if_enqueue(ifp, m)		pie80211_if_enqueue(ifp, m)
#define if_start(ifp)			pie80211_if_start(ifp)
#define if_link_state_change(ifp)	pie80211_if_link_state_change(ifp)
#define ether_ifattach(ifp)		pie80211_if_attach(ifp)
#define ether_ifdetach(ifp)		pie80211_if_detach(ifp)
#define ether_output(ifp, m, dst, rt)	pie80211_ether_output(ifp, m, dst, rt)
void pie80211_if_attach(struct ifnet *ifp);
void pie80211_if_detach(struct ifnet *ifp);
void pie80211_if_input(struct ifnet *ifp, struct mbuf_list *ml);
int pie80211_if_enqueue(struct ifnet *ifp, struct mbuf *m);
void pie80211_if_start(struct ifnet *ifp);
void pie80211_if_link_state_change(struct ifnet *ifp);
int pie80211_ether_output(struct ifnet *, struct mbuf *, struct sockaddr *, struct rtentry *);

#endif
