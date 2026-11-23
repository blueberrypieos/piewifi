/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "netfile.h"

static void *(*const volatile wipe_fn)(void *, int, size_t) = memset;

void
net_wipe(void *p, size_t n)
{
	wipe_fn(p, 0, n);
}

static int
unhex(const char *s, uint8_t *out, size_t cap, size_t *len)
{
	size_t n = strlen(s), i;
	unsigned int v;

	if (n == 0 || n % 2 != 0 || n / 2 > cap)
		return -1;
	for (i = 0; i < n / 2; i++) {
		if (sscanf(s + 2 * i, "%2x", &v) != 1 || strchr("0123456789abcdefABCDEF", s[2 * i]) == NULL ||
		    strchr("0123456789abcdefABCDEF", s[2 * i + 1]) == NULL)
			return -1;
		out[i] = (uint8_t)v;
	}
	*len = n / 2;
	return 0;
}

static size_t
tohex(const uint8_t *p, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = digits[p[i] >> 4];
		out[2 * i + 1] = digits[p[i] & 15];
	}
	out[2 * n] = '\0';
	return 2 * n;
}

int
net_passphrase_ok(const char *pass)
{
	size_t n = strlen(pass), i;

	if (n < 8 || n > 63)
		return 0;
	for (i = 0; i < n; i++)
		if (pass[i] < 0x20 || pass[i] > 0x7e)
			return 0;
	return 1;
}

int
net_parse_line(const char *line, struct net *n)
{
	char copy[512], *field[5], *p, *save = NULL;
	char *end;
	long prio;
	int nf = 0, rc = -1;

	if (strlen(line) >= sizeof(copy))
		return -1;
	strcpy(copy, line);
	for (p = strtok_r(copy, " \t\r\n", &save); p != NULL && nf < 5; p = strtok_r(NULL, " \t\r\n", &save))
		field[nf++] = p;
	if (nf == 0 || field[0][0] == '#') {
		rc = 1;
		goto out;
	}
	if (nf != 4)
		goto out;
	memset(n, 0, sizeof(*n));
	if (unhex(field[0], n->ssid, NET_SSID_MAX, &n->ssid_len) != 0)
		goto out;
	errno = 0;
	prio = strtol(field[3], &end, 10);
	if (errno != 0 || *end != '\0' || prio < 0 || prio > 255)
		goto out;
	n->priority = (int)prio;
	if (strcmp(field[1], "open") == 0 && strcmp(field[2], "-") == 0) {
		n->kind = NET_OPEN;
		rc = 0;
	} else if (strcmp(field[1], "psk") == 0) {
		n->kind = NET_PSK;
		if (unhex(field[2], n->secret, NET_SECRET_MAX, &n->secret_len) == 0 && n->secret_len == 32)
			rc = 0;
	} else if (strcmp(field[1], "passphrase") == 0) {
		n->kind = NET_PASSPHRASE;
		if (unhex(field[2], n->secret, 63, &n->secret_len) == 0) {
			char pass[64];

			memcpy(pass, n->secret, n->secret_len);
			pass[n->secret_len] = '\0';
			if (net_passphrase_ok(pass))
				rc = 0;
			net_wipe(pass, sizeof(pass));
		}
	}
	if (rc != 0)
		net_wipe(n, sizeof(*n));
out:
	net_wipe(copy, sizeof(copy));
	return rc;
}

int
net_format_line(const struct net *n, char *out, size_t cap)
{
	static const char *const kinds[] = { "open", "psk", "passphrase" };
	char ssid[2 * NET_SSID_MAX + 1], secret[2 * NET_SECRET_MAX + 1];
	int len;

	if (n->ssid_len == 0 || n->ssid_len > NET_SSID_MAX || n->secret_len > NET_SECRET_MAX ||
	    (unsigned)n->kind > NET_PASSPHRASE)
		return -1;
	tohex(n->ssid, n->ssid_len, ssid);
	if (n->kind == NET_OPEN)
		strcpy(secret, "-");
	else
		tohex(n->secret, n->secret_len, secret);
	len = snprintf(out, cap, "%s %s %s %d\n", ssid, kinds[n->kind], secret, n->priority);
	net_wipe(secret, sizeof(secret));
	return len < 0 || (size_t)len >= cap ? -1 : len;
}

int
net_read(const char *path, struct net *nets, size_t max, size_t *count, size_t *badline)
{
	char line[512], iobuf[BUFSIZ];
	size_t lineno = 0;
	FILE *f;
	int rc = 0, r, error = 0;

	*count = 0;
	*badline = 0;
	f = fopen(path, "r");
	if (f == NULL)
		return errno == ENOENT ? 0 : -1;
	setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));
	for (;;) {
		errno = 0;
		if (fgets(line, sizeof(line), f) == NULL || ferror(f))
			break;
		lineno++;
		if (*count == max) {
			rc = -1;
			*badline = lineno;
			break;
		}
		r = net_parse_line(line, &nets[*count]);
		if (r < 0) {
			rc = -1;
			*badline = lineno;
			break;
		}
		if (r == 0)
			(*count)++;
	}
	if (rc == 0 && ferror(f)) {
		error = errno != 0 ? errno : EIO;
		rc = -1;
	}
	net_wipe(line, sizeof(line));
	fclose(f);
	net_wipe(iobuf, sizeof(iobuf));
	if (error != 0)
		errno = error;
	return rc;
}

/* text replaces the file at path, through a temporary file of its own that is synced and renamed. */
static int
replace_file(const char *path, const char *text, size_t len)
{
	char tmp[PATH_MAX], dir[PATH_MAX], *slash;
	int fd, rc = -1;

	if (snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path) >= (int)sizeof(tmp) ||
	    snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir))
		return -1;
	if ((slash = strrchr(dir, '/')) != NULL && slash != dir) {
		*slash = '\0';
		if (mkdir(dir, 0700) != 0 && errno != EEXIST)
			return -1;
	}
	fd = mkstemp(tmp);
	if (fd < 0)
		return -1;
	if (len > 0 && write(fd, text, len) != (ssize_t)len)
		goto fail;
	if (fsync(fd) != 0)
		goto fail;
	close(fd);
	fd = -1;
	if (rename(tmp, path) != 0)
		goto fail;
	if (slash != NULL && (fd = open(slash != dir ? dir : "/", O_RDONLY)) >= 0) {
		fsync(fd);
		close(fd);
	}
	rc = 0;
	goto out;
fail:
	rc = errno;
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	errno = rc;
	rc = -1;
out:
	return rc;
}

int
net_write(const char *path, const struct net *nets, size_t count)
{
	char text[NET_MAX * 512];
	size_t i, used = 0;
	int len, rc = -1;

	if (count > NET_MAX) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < count; i++) {
		len = net_format_line(&nets[i], text + used, sizeof(text) - used);
		if (len < 0) {
			errno = EINVAL;
			goto out;
		}
		used += (size_t)len;
	}
	rc = replace_file(path, text, used);
out:
	net_wipe(text, sizeof(text));
	return rc;
}

int
net_write_text(const char *path, const char *text, size_t len)
{
	return replace_file(path, text, len);
}

int
net_lock(const char *path)
{
	char lock[PATH_MAX], *slash;
	int fd;

	if (snprintf(lock, sizeof(lock), "%s.lock", path) >= (int)sizeof(lock))
		return -1;
	if ((slash = strrchr(lock, '/')) != NULL && slash != lock) {
		*slash = '\0';
		if (mkdir(lock, 0700) != 0 && errno != EEXIST)
			return -1;
		*slash = '/';
	}
	fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	while (flock(fd, LOCK_EX) != 0) {
		if (errno != EINTR) {
			close(fd);
			return -1;
		}
	}
	return fd;
}

int
net_find(const struct net *nets, size_t count, const uint8_t *ssid, size_t len)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (nets[i].ssid_len == len && memcmp(nets[i].ssid, ssid, len) == 0)
			return (int)i;
	return -1;
}

int
net_import_parse(const char *text, size_t textlen, struct net *nets, size_t max, size_t *count)
{
	const char *p = text, *eol, *val;
	size_t len, vlen;
	struct net *cur = NULL;
	char pass[64];

	*count = 0;
	if (memchr(text, '\0', textlen) != NULL)
		return -1;
	if (textlen >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0)
		p += 3;
	for (; *p != '\0'; p = *eol != '\0' ? eol + 1 : eol) {
		eol = strchr(p, '\n');
		if (eol == NULL)
			eol = p + strlen(p);
		len = (size_t)(eol - p);
		if (len > 0 && p[len - 1] == '\r')
			len--;
		if (len == 0 || p[0] == '#')
			continue;
		if (len > 5 && strncmp(p, "ssid=", 5) == 0) {
			val = p + 5;
			vlen = len - 5;
			if (*count == max || vlen > NET_SSID_MAX)
				return -1;
			cur = &nets[(*count)++];
			memset(cur, 0, sizeof(*cur));
			memcpy(cur->ssid, val, vlen);
			cur->ssid_len = vlen;
			cur->kind = NET_OPEN;
		} else if (len > 11 && strncmp(p, "passphrase=", 11) == 0) {
			val = p + 11;
			vlen = len - 11;
			if (cur == NULL || cur->kind != NET_OPEN || vlen > 63)
				return -1;
			memcpy(pass, val, vlen);
			pass[vlen] = '\0';
			if (!net_passphrase_ok(pass)) {
				net_wipe(pass, sizeof(pass));
				return -1;
			}
			memcpy(cur->secret, pass, vlen);
			cur->secret_len = vlen;
			cur->kind = NET_PASSPHRASE;
			net_wipe(pass, sizeof(pass));
		} else {
			return -1;
		}
	}
	return 0;
}
