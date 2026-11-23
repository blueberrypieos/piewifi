/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
/*
 * net_read fails on a file that does not read to its end, as on a bad
 * line, and a read error is not taken for a bad line. net_write replaces
 * the file through a temporary file of its own, synced and renamed, and
 * net_write_text does the same with any text. net_lock takes the lock
 * that a read, change and write of the file holds from start to end: an
 * flock of the file's .lock file, which stays. It returns the descriptor
 * that holds it, or -1. net_import_parse refuses a text with a NUL byte in
 * its len bytes, and skips a UTF-8 byte order mark at its start.
 */
#ifndef _NETFILE_H
#define _NETFILE_H

#include <stddef.h>
#include <stdint.h>

#define NET_SSID_MAX	32
#define NET_SECRET_MAX	64
#define NET_MAX		16

enum net_kind { NET_OPEN, NET_PSK, NET_PASSPHRASE };

struct net {
	uint8_t ssid[NET_SSID_MAX];
	size_t ssid_len;
	enum net_kind kind;
	uint8_t secret[NET_SECRET_MAX];
	size_t secret_len;
	int priority;
};

int net_parse_line(const char *line, struct net *n);
int net_format_line(const struct net *n, char *out, size_t cap);
int net_read(const char *path, struct net *nets, size_t max, size_t *count, size_t *badline);
int net_write(const char *path, const struct net *nets, size_t count);
int net_write_text(const char *path, const char *text, size_t len);
int net_lock(const char *path);
int net_find(const struct net *nets, size_t count, const uint8_t *ssid, size_t len);
int net_passphrase_ok(const char *pass);
int net_import_parse(const char *text, size_t len, struct net *nets, size_t max, size_t *count);
void net_wipe(void *p, size_t n);

#endif
