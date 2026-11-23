/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * usage: test_netfile DIR
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "netfile.h"

static int failures;

#define CHECK(e)							\
	do {								\
		if (!(e)) {						\
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); \
			failures++;					\
		}							\
	} while (0)

static const char psk_hex[] =
    "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e";

static void
test_lines(void)
{
	struct net n, back;
	char line[512];

	CHECK(net_parse_line("486f6d65 open - 3\n", &n) == 0 && n.kind == NET_OPEN &&
	    n.ssid_len == 4 && memcmp(n.ssid, "Home", 4) == 0 && n.priority == 3);
	snprintf(line, sizeof(line), "49454545 psk %s 0", psk_hex);
	CHECK(net_parse_line(line, &n) == 0 && n.kind == NET_PSK && n.secret_len == 32 &&
	    n.secret[0] == 0xf4 && n.secret[31] == 0x2e);
	CHECK(net_format_line(&n, line, sizeof(line)) > 0 && net_parse_line(line, &back) == 0 &&
	    memcmp(&n, &back, sizeof(n)) == 0);
	CHECK(net_parse_line("4945 passphrase 70617373776f7264 1", &n) == 0 &&
	    n.kind == NET_PASSPHRASE && n.secret_len == 8 && memcmp(n.secret, "password", 8) == 0);
	CHECK(net_format_line(&n, line, sizeof(line)) > 0 && net_parse_line(line, &back) == 0 &&
	    memcmp(&n, &back, sizeof(n)) == 0);
	CHECK(net_parse_line("", &n) == 1);
	CHECK(net_parse_line("   \n", &n) == 1);
	CHECK(net_parse_line("# a comment", &n) == 1);
	CHECK(net_parse_line("486f6d65 open -", &n) == -1);
	CHECK(net_parse_line("486f6d65 open - 3 4", &n) == -1);
	CHECK(net_parse_line("486f6d6 open - 3", &n) == -1);
	CHECK(net_parse_line("486f6d6g open - 3", &n) == -1);
	CHECK(net_parse_line("486f6d65 open 00 3", &n) == -1);
	CHECK(net_parse_line("486f6d65 open - 256", &n) == -1);
	CHECK(net_parse_line("486f6d65 open - -1", &n) == -1);
	CHECK(net_parse_line("486f6d65 wep 0011223344 3", &n) == -1);
	CHECK(net_parse_line("486f6d65 psk 0011 3", &n) == -1);
	CHECK(net_parse_line("486f6d65 passphrase 73686f7274 3", &n) == -1);
	snprintf(line, sizeof(line), "%s%s open - 0", psk_hex, "00");
	CHECK(net_parse_line(line, &n) == -1);
}

static void
test_files(const char *top)
{
	struct net nets[3], back[NET_MAX];
	char path[1024], dir[512], text[16];
	struct stat sb;
	size_t count, bad;
	FILE *f;

	memset(nets, 0, sizeof(nets));
	CHECK(net_parse_line("41 open - 0", &nets[0]) == 0);
	CHECK(net_parse_line("42 passphrase 70617373776f7264 2", &nets[1]) == 0);
	snprintf(path, sizeof(path), "43 psk %s 9", psk_hex);
	CHECK(net_parse_line(path, &nets[2]) == 0);
	snprintf(dir, sizeof(dir), "%s/runXXXXXX", top);
	CHECK(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/wifi/networks", dir);
	CHECK(net_read(path, back, NET_MAX, &count, &bad) == 0 && count == 0);
	CHECK(net_write(path, nets, 3) == 0);
	CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600);
	CHECK(net_read(path, back, NET_MAX, &count, &bad) == 0 && count == 3);
	CHECK(memcmp(back, nets, sizeof(nets)) == 0);
	CHECK(net_find(back, count, (const uint8_t *)"B", 1) == 1);
	CHECK(net_find(back, count, (const uint8_t *)"D", 1) == -1);
	f = fopen(path, "a");
	CHECK(f != NULL);
	if (f != NULL) {
		fputs("44 psk 00 1\n", f);
		fclose(f);
	}
	CHECK(net_read(path, back, NET_MAX, &count, &bad) == -1 && bad == 4);
	CHECK(net_read(path, back, 2, &count, &bad) == -1 && bad == 3);
	errno = 0;
	CHECK(net_read(dir, back, NET_MAX, &count, &bad) == -1 && bad == 0 && errno == EISDIR);

	snprintf(path, sizeof(path), "%s/wifi/country", dir);
	CHECK(net_write_text(path, "DE\n", 3) == 0);
	CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600 && sb.st_size == 3);
	CHECK(net_write_text(path, "FR\n", 3) == 0);
	f = fopen(path, "r");
	CHECK(f != NULL && fgets(text, sizeof(text), f) != NULL && strcmp(text, "FR\n") == 0);
	if (f != NULL)
		fclose(f);
}

static int
files_in(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
			n++;
	closedir(d);
	return n;
}

static void
test_writers(const char *top)
{
	struct net nets[1], back[NET_MAX];
	char dir[512], path[1024], other[1100], buf[16];
	struct pollfd pfd;
	size_t count, bad;
	int fds[2], lock, status;
	pid_t pid;
	FILE *f;

	snprintf(dir, sizeof(dir), "%s/lockXXXXXX", top);
	CHECK(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/wifi/networks", dir);
	snprintf(other, sizeof(other), "%s.new", path);
	CHECK(net_parse_line("41 open - 0", &nets[0]) == 0);
	CHECK(net_write(path, nets, 1) == 0);
	f = fopen(other, "w");
	CHECK(f != NULL);
	if (f != NULL) {
		fputs("half written", f);
		fclose(f);
	}
	CHECK(net_write(path, nets, 1) == 0);
	f = fopen(other, "r");
	CHECK(f != NULL && fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "half written") == 0);
	if (f != NULL)
		fclose(f);
	CHECK(net_read(path, back, NET_MAX, &count, &bad) == 0 && count == 1);
	snprintf(other, sizeof(other), "%s/wifi", dir);
	CHECK(files_in(other) == 2);

	lock = net_lock(path);
	CHECK(lock >= 0 && files_in(other) == 3);
	CHECK(pipe(fds) == 0);
	pid = fork();
	if (pid == 0) {
		close(fds[0]);
		close(lock);
		lock = net_lock(path);
		_exit(write(fds[1], lock >= 0 ? "y" : "n", 1) == 1 ? 0 : 1);
	}
	close(fds[1]);
	pfd.fd = fds[0];
	pfd.events = POLLIN;
	CHECK(poll(&pfd, 1, 300) == 0);
	close(lock);
	if (poll(&pfd, 1, 5000) == 1 && read(fds[0], buf, 1) == 1) {
		CHECK(buf[0] == 'y');
	} else {
		CHECK(!"the second lock came");
		kill(pid, SIGKILL);
	}
	close(fds[0]);
	CHECK(waitpid(pid, &status, 0) == pid);
}

static void
test_import(void)
{
	struct net nets[NET_MAX];
	size_t count;
	char big[64];

	static const char nul[] = "ssid=Home\0\npassphrase=password\n";
	static const char bom[] = "\xef\xbb\xbfssid=Home\npassphrase=password\n";
	static const char ok[] = "ssid=Home\r\npassphrase=password\r\n\r\n# x\nssid=Cafe\n";

	CHECK(net_import_parse(ok, sizeof(ok) - 1, nets, NET_MAX, &count) == 0 && count == 2);
	CHECK(nets[0].kind == NET_PASSPHRASE && nets[0].ssid_len == 4 &&
	    memcmp(nets[0].secret, "password", 8) == 0 && nets[0].secret_len == 8);
	CHECK(nets[1].kind == NET_OPEN && memcmp(nets[1].ssid, "Cafe", 4) == 0);
	CHECK(net_import_parse(nul, sizeof(nul) - 1, nets, NET_MAX, &count) == -1);
	CHECK(net_import_parse(nul, 9, nets, NET_MAX, &count) == 0 && count == 1);
	CHECK(net_import_parse("passphrase=password\n", 20, nets, NET_MAX, &count) == -1);
	CHECK(net_import_parse("ssid=A\npassphrase=short\n", 24, nets, NET_MAX, &count) == -1);
	CHECK(net_import_parse("ssid=A\npassphrase=password\npassphrase=password\n", 47, nets,
	    NET_MAX, &count) == -1);
	CHECK(net_import_parse("ssid=A\nkey=password\n", 20, nets, NET_MAX, &count) == -1);
	memset(big, 'x', sizeof(big));
	memcpy(big, "ssid=", 5);
	big[5 + 33] = '\0';
	CHECK(net_import_parse(big, 5 + 33, nets, NET_MAX, &count) == -1);
	CHECK(net_import_parse("ssid=A\nssid=B\n", 14, nets, 1, &count) == -1);
	CHECK(net_import_parse(bom, sizeof(bom) - 1, nets, NET_MAX, &count) == 0 && count == 1 &&
	    nets[0].ssid_len == 4 && memcmp(nets[0].ssid, "Home", 4) == 0);
	CHECK(net_import_parse("\xef\xbbssid=A\n", 9, nets, NET_MAX, &count) == -1);
}

int
main(int argc, char *argv[])
{
	if (argc != 2)
		return 2;
	test_lines();
	test_files(argv[1]);
	test_writers(argv[1]);
	test_import();
	printf("test_netfile: %s\n", failures == 0 ? "ok" : "FAILED");
	return failures != 0;
}
