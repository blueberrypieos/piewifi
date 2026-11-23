/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <pie80211/compat.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>
#include <crypto/md5.h>
#include "crypto/sha1.h"
#include "crypto/sha2.h"
#include "crypto/aes.h"
#include <crypto/hmac.h>
#include <crypto/cmac.h>
#include <crypto/key_wrap.h>
#include "host.h"

void ieee80211_prf(const u_int8_t *, size_t, const u_int8_t *, size_t, const u_int8_t *,
    size_t, u_int8_t *, size_t);

static struct ieee80211com ic;

static size_t
unhex(const char *s, uint8_t *out)
{
	size_t n = 0;
	unsigned int v;

	while (s[0] != '\0' && s[1] != '\0' && sscanf(s, "%2x", &v) == 1) {
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static int
same(const uint8_t *got, const char *hex)
{
	uint8_t want[512];
	size_t n = unhex(hex, want);

	return memcmp(got, want, n) == 0;
}

static void
test_aes(void)
{
	uint8_t key[16], in[16], out[16];
	AES_CTX ctx;

	unhex("000102030405060708090a0b0c0d0e0f", key);
	unhex("00112233445566778899aabbccddeeff", in);
	CHECK(AES_Setkey(&ctx, key, 16) == 0);
	AES_Encrypt(&ctx, in, out);
	CHECK(same(out, "69c4e0d86a7b0430d8cdb78070b4c55a"));
}

static void
test_sha(void)
{
	uint8_t out[32];
	HMAC_SHA1_CTX h;
	SHA2_CTX s;
	uint8_t key[20];

	SHA256Init(&s);
	SHA256Update(&s, "abc", 3);
	SHA256Final(out, &s);
	CHECK(same(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	memset(key, 0x0b, sizeof(key));
	HMAC_SHA1_Init(&h, key, sizeof(key));
	HMAC_SHA1_Update(&h, (const uint8_t *)"Hi There", 8);
	HMAC_SHA1_Final(out, &h);
	CHECK(same(out, "b617318655057264e28bc0b6fb378c8ef146be00"));
	HMAC_SHA1_Init(&h, (const uint8_t *)"Jefe", 4);
	HMAC_SHA1_Update(&h, (const uint8_t *)"what do ya want for nothing?", 28);
	HMAC_SHA1_Final(out, &h);
	CHECK(same(out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
}

static void
test_cmac_wrap(void)
{
	uint8_t key[16], msg[16], out[24], back[16];
	AES_CMAC_CTX c;
	aes_key_wrap_ctx w;

	unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
	unhex("6bc1bee22e409f96e93d7e117393172a", msg);
	AES_CMAC_Init(&c);
	AES_CMAC_SetKey(&c, key);
	AES_CMAC_Update(&c, msg, 16);
	AES_CMAC_Final(out, &c);
	CHECK(same(out, "070a16b46b4d4144f79bdd9dd04a287c"));

	unhex("000102030405060708090a0b0c0d0e0f", key);
	unhex("00112233445566778899aabbccddeeff", msg);
	aes_key_wrap_set_key(&w, key, 16);
	aes_key_wrap(&w, msg, 2, out);
	CHECK(same(out, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
	CHECK(aes_key_unwrap(&w, out, back, 2) == 0 && memcmp(back, msg, 16) == 0);
	out[5] ^= 1;
	CHECK(aes_key_unwrap(&w, out, back, 2) != 0);
}

static void
test_prf_ptk(void)
{
	uint8_t key[20], out[64], pmk[32], aa[6], spa[6], an[32], sn[32];
	struct ieee80211_ptk ptk;
	int i;

	memset(key, 0x0b, sizeof(key));
	ieee80211_prf(key, sizeof(key), (const uint8_t *)"prefix", 7,
	    (const uint8_t *)"Hi There", 8, out, 64);
	CHECK(same(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
	    "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));

	unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", pmk);
	unhex("020000000001", aa);
	unhex("020000000002", spa);
	for (i = 0; i < 32; i++) {
		an[i] = (uint8_t)(0x20 + i);
		sn[i] = (uint8_t)(0x40 + i);
	}
	CHECK(sizeof(ptk) == 64);
	ieee80211_derive_ptk(IEEE80211_AKM_PSK, pmk, aa, spa, an, sn, &ptk);
	CHECK(same((const uint8_t *)&ptk, "287711753cb0c0dec2b4043f3e1c9ba018baac0b9d74a10ffe9c213903bc3492"
	    "1c4a2eaf072cf18bde99aad6a96252c9cdded3eefd07d98f4757eedae85cb39a"));
	ieee80211_derive_ptk(IEEE80211_AKM_PSK, pmk, spa, aa, sn, an, &ptk);
	CHECK(same((const uint8_t *)&ptk, "287711753cb0c0dec2b4043f3e1c9ba0"));
}

static const char ccmp_in[] =
    "084100000200000000010200000000020200000000031000aaaa030000000800"
    "000306090c0f1215181b1e2124272a2d303336393c3f4245484b4e5154575a5d";
static const char ccmp_out[] =
    "0841000002000000000102000000000202000000000310000100002000000000"
    "cc4b3ab7f00358960e6ae8adb575c5502e2c9425001e9b28d3cedd8f2dd32c8f"
    "907e92763dd96557353f652d2ca381dc";

static void
test_ccmp(void)
{
	struct ieee80211_key k;
	uint8_t frame[128], out[128];
	struct mbuf *m, *enc, *dec;
	size_t inlen, outlen;
	int i;

	memset(&k, 0, sizeof(k));
	k.k_cipher = IEEE80211_CIPHER_CCMP;
	k.k_len = 16;
	for (i = 0; i < 16; i++)
		k.k_key[i] = (uint8_t)(0xa0 + i);
	CHECK(ieee80211_ccmp_set_key(&ic, &k) == 0);

	inlen = unhex(ccmp_in, frame);
	outlen = unhex(ccmp_out, out);
	m = pie80211_m_frombuf(frame, inlen, 0);
	enc = ieee80211_ccmp_encrypt(&ic, m, &k);
	CHECK(enc != NULL && pie80211_m_pktlen(enc) == outlen);
	CHECK(enc != NULL && pie80211_m_tobuf(enc, frame, sizeof(frame)) == (int)outlen &&
	    memcmp(frame, out, outlen) == 0);
	if (memcmp(frame, out, outlen) != 0) {
		for (i = 0; i < (int)outlen; i++)
			fprintf(stderr, "%02x%s", frame[i], i % 32 == 31 ? "\n" : "");
		fputc('\n', stderr);
	}

	dec = ieee80211_ccmp_decrypt(&ic, m_dup_pkt(enc, 0, M_DONTWAIT), &k);
	CHECK(dec != NULL && pie80211_m_pktlen(dec) == inlen);
	unhex(ccmp_in, out);
	out[1] |= IEEE80211_FC1_PROTECTED;
	CHECK(dec != NULL && pie80211_m_tobuf(dec, frame, sizeof(frame)) == (int)inlen);
	CHECK(memcmp(frame + 2, out + 2, inlen - 2) == 0);
	m_freem(dec);

	CHECK(ieee80211_ccmp_decrypt(&ic, m_dup_pkt(enc, 0, M_DONTWAIT), &k) == NULL);
	CHECK(ic.ic_stats.is_ccmp_replays == 1);

	k.k_rsc[0] = 0;
	m = m_dup_pkt(enc, 0, M_DONTWAIT);
	mtod(m, uint8_t *)[40] ^= 0x01;
	CHECK(ieee80211_ccmp_decrypt(&ic, m, &k) == NULL);
	CHECK(ic.ic_stats.is_ccmp_dec_errs == 1);
	m_freem(enc);
	ieee80211_ccmp_delete_key(&ic, &k);
	CHECK(host_live() == 0);
}

int
main(void)
{
	test_aes();
	test_sha();
	test_cmac_wrap();
	test_prf_ptk();
	test_ccmp();
	return host_report("test_crypto");
}
