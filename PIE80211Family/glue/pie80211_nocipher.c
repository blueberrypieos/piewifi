/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include <net80211/ieee80211_var.h>
#include <crypto/arc4.h>

void
rc4_keysetup(struct rc4_ctx *state, u_char *secret, u_int32_t n)
{
	explicit_bzero(state, sizeof(struct rc4_ctx));
}

void
rc4_skip(struct rc4_ctx *state, u_int32_t n)
{
}

void
rc4_crypt(struct rc4_ctx *state, u_char *in, u_char *out, u_int32_t n)
{
	explicit_bzero(out, n);
}

int
ieee80211_wep_set_key(struct ieee80211com *com, struct ieee80211_key *key)
{
	return EOPNOTSUPP;
}

void
ieee80211_wep_delete_key(struct ieee80211com *com, struct ieee80211_key *key)
{
}

struct mbuf *
ieee80211_wep_encrypt(struct ieee80211com *com, struct mbuf *m0, struct ieee80211_key *key)
{
	m_freem(m0);
	return NULL;
}

struct mbuf *
ieee80211_wep_decrypt(struct ieee80211com *com, struct mbuf *m0, struct ieee80211_key *key)
{
	m_freem(m0);
	return NULL;
}

int
ieee80211_tkip_set_key(struct ieee80211com *com, struct ieee80211_key *key)
{
	return EOPNOTSUPP;
}

void
ieee80211_tkip_delete_key(struct ieee80211com *com, struct ieee80211_key *key)
{
}

struct mbuf *
ieee80211_tkip_encrypt(struct ieee80211com *com, struct mbuf *m0, struct ieee80211_key *key)
{
	m_freem(m0);
	return NULL;
}

int
ieee80211_tkip_get_tsc(uint64_t *pn, uint64_t **last, struct mbuf *m0,
    struct ieee80211_key *key)
{
	return EOPNOTSUPP;
}

struct mbuf *
ieee80211_tkip_decrypt(struct ieee80211com *com, struct mbuf *m0, struct ieee80211_key *key)
{
	m_freem(m0);
	return NULL;
}

void
ieee80211_michael_mic_failure(struct ieee80211com *com, u_int64_t counter)
{
}

int
ieee80211_bip_set_key(struct ieee80211com *com, struct ieee80211_key *key)
{
	return EOPNOTSUPP;
}

void
ieee80211_bip_delete_key(struct ieee80211com *com, struct ieee80211_key *key)
{
}

struct mbuf *
ieee80211_bip_encap(struct ieee80211com *com, struct mbuf *m0, struct ieee80211_key *key)
{
	m_freem(m0);
	return NULL;
}

struct mbuf *
ieee80211_bip_decap(struct ieee80211com *com, struct mbuf *m0, struct ieee80211_key *key)
{
	m_freem(m0);
	return NULL;
}
