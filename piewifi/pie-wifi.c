/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 *
 * usage: pie-wifi [-i interface] command [argument]
 *   scan                 scans, then lists the networks in range
 *   join SSID            stores a WPA2 network; asks for its passphrase
 *   join SSID --open     stores an open network
 *   forget SSID          removes a stored network
 *   list                 lists the stored networks
 *   status               shows the link
 *   country XX           sets and stores the regulatory country
 *   load                 gives the stored networks and country to the kernel
 *   import FILE          stores the networks of FILE, then deletes FILE
 *   wait SECONDS         waits until the link is up
 *   watch                loads each WiFi interface as it attaches; for rc
 *
 * Without -i, the interface is the first one that answers
 * PIE80211Family's status command.
 */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/kern_event.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/sys_domain.h>
#include <net/if.h>
#include <net/net_kev.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <readpassphrase.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <pie80211/ioctl.h>
#include "netfile.h"

#define WIFI_DIR	"/var/db/wifi"
#define NETWORKS	WIFI_DIR "/networks"
#define COUNTRY		WIFI_DIR "/country"
#define COUNTRY_FILE	1
#define COUNTRY_LOCK	2
#define WATCH_LOCK	"/var/run/pie-wifi.watch"
#define WATCH_MAX	16
#define WATCH_COUNTRY	1
#define WATCH_NETWORKS	2
#define IMPORT_KEPT	3

static const char *ifname;
static char picked[IFNAMSIZ];

static void
usage(void)
{
	fprintf(stderr, "usage: pie-wifi [-i interface] scan | join SSID [--open] | forget SSID |\n"
	    "       list | status | country XX | load | import FILE | wait SECONDS | watch\n");
	exit(2);
}

static int
drv_on(const char *name, unsigned long cmd, void *data, size_t len, int set)
{
	struct ifdrv ifd;
	int s, rc;

	memset(&ifd, 0, sizeof(ifd));
	strlcpy(ifd.ifd_name, name, sizeof(ifd.ifd_name));
	ifd.ifd_cmd = cmd;
	ifd.ifd_len = len;
	ifd.ifd_data = data;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	rc = ioctl(s, set ? SIOCSDRVSPEC : SIOCGDRVSPEC, &ifd);
	close(s);
	return rc;
}

static int
is_wifi(const char *name)
{
	struct pie80211_status st;

	memset(&st, 0, sizeof(st));
	return drv_on(name, PIE80211_CMD_STATUS, &st, sizeof(st), 0) == 0;
}

static const char *
wifi_if(void)
{
	struct if_nameindex *ifs, *p;

	if (ifname != NULL)
		return ifname;
	ifs = if_nameindex();
	for (p = ifs; p != NULL && p->if_index != 0; p++) {
		if (is_wifi(p->if_name)) {
			strlcpy(picked, p->if_name, sizeof(picked));
			ifname = picked;
			break;
		}
	}
	if (ifs != NULL)
		if_freenameindex(ifs);
	return ifname;
}

static const char *
wname(void)
{
	return ifname != NULL ? ifname : "no WiFi interface";
}

static int
drv(unsigned long cmd, void *data, size_t len, int set)
{
	if (wifi_if() == NULL) {
		errno = ENXIO;
		return -1;
	}
	return drv_on(ifname, cmd, data, len, set);
}

static void
print_ssid(const uint8_t *ssid, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (isprint(ssid[i]) && ssid[i] != '\\')
			putchar(ssid[i]);
		else
			printf("\\x%02x", ssid[i]);
	}
}

static void
derive(const uint8_t *ssid, size_t ssid_len, const char *pass, uint8_t psk[PIE80211_PSK_LEN])
{
	if (PKCS5_PBKDF2_HMAC_SHA1(pass, (int)strlen(pass), ssid, (int)ssid_len, 4096,
	    PIE80211_PSK_LEN, psk) != 1)
		errx(1, "cannot derive the key");
}

static void
to_psk(struct net *n)
{
	uint8_t psk[PIE80211_PSK_LEN];
	char pass[64];

	if (n->kind != NET_PASSPHRASE)
		return;
	memcpy(pass, n->secret, n->secret_len);
	pass[n->secret_len] = '\0';
	derive(n->ssid, n->ssid_len, pass, psk);
	net_wipe(pass, sizeof(pass));
	net_wipe(n->secret, sizeof(n->secret));
	memcpy(n->secret, psk, sizeof(psk));
	n->secret_len = sizeof(psk);
	n->kind = NET_PSK;
	net_wipe(psk, sizeof(psk));
}

static size_t
load_networks(struct net *nets)
{
	size_t count, bad;

	if (net_read(NETWORKS, nets, NET_MAX, &count, &bad) != 0) {
		if (bad != 0)
			errx(1, "%s: line %zu is not a network", NETWORKS, bad);
		err(1, "%s", NETWORKS);
	}
	return count;
}

static void
save_networks(const struct net *nets, size_t count)
{
	if (net_write(NETWORKS, nets, count) != 0)
		err(1, "%s", NETWORKS);
}

static int
lock_networks(void)
{
	int fd = net_lock(NETWORKS);

	if (fd < 0)
		err(1, "%s.lock", NETWORKS);
	return fd;
}

static int
push(const struct net *n)
{
	struct pie80211_join_req req;
	struct net copy = *n;
	int rc;

	to_psk(&copy);
	memset(&req, 0, sizeof(req));
	req.ssid.len = (uint8_t)copy.ssid_len;
	memcpy(req.ssid.bytes, copy.ssid, copy.ssid_len);
	req.priority = (uint8_t)copy.priority;
	req.security = copy.kind == NET_OPEN ? PIE80211_SEC_OPEN : PIE80211_SEC_WPA2_PSK;
	if (copy.kind == NET_PSK)
		memcpy(req.psk, copy.secret, PIE80211_PSK_LEN);
	rc = drv(PIE80211_CMD_JOIN_ADD, &req, sizeof(req), 1);
	net_wipe(&req, sizeof(req));
	net_wipe(&copy, sizeof(copy));
	return rc;
}

static void
put(struct net *nets, size_t *count, const struct net *n)
{
	int i = net_find(nets, *count, n->ssid, n->ssid_len);

	if (i < 0) {
		if (*count == NET_MAX)
			errx(1, "%s holds %d networks already", NETWORKS, NET_MAX);
		i = (int)(*count)++;
	}
	net_wipe(&nets[i], sizeof(nets[i]));
	nets[i] = *n;
}

static int
cmd_join(const char *ssid, int open)
{
	struct net nets[NET_MAX], n;
	size_t count;
	char pass[128];
	int lock;

	memset(&n, 0, sizeof(n));
	n.ssid_len = strlen(ssid);
	if (n.ssid_len == 0 || n.ssid_len > NET_SSID_MAX)
		errx(1, "an SSID has 1 to %d bytes", NET_SSID_MAX);
	memcpy(n.ssid, ssid, n.ssid_len);
	n.kind = NET_OPEN;
	if (!open) {
		if (readpassphrase("Passphrase: ", pass, sizeof(pass), RPP_REQUIRE_TTY) == NULL)
			err(1, "cannot read the passphrase");
		if (!net_passphrase_ok(pass)) {
			net_wipe(pass, sizeof(pass));
			errx(1, "a passphrase has 8 to 63 printable characters");
		}
		derive(n.ssid, n.ssid_len, pass, n.secret);
		net_wipe(pass, sizeof(pass));
		n.secret_len = PIE80211_PSK_LEN;
		n.kind = NET_PSK;
	}
	lock = lock_networks();
	count = load_networks(nets);
	put(nets, &count, &n);
	save_networks(nets, count);
	if (push(&n) != 0)
		warn("%s", wname());
	close(lock);
	net_wipe(nets, sizeof(nets));
	net_wipe(&n, sizeof(n));
	return 0;
}

static int
cmd_forget(const char *ssid)
{
	struct net nets[NET_MAX];
	struct pie80211_join_req req;
	size_t count, len = strlen(ssid);
	int i, lock;

	lock = lock_networks();
	count = load_networks(nets);
	i = net_find(nets, count, (const uint8_t *)ssid, len);
	if (i < 0)
		errx(1, "%s is not stored", ssid);
	net_wipe(&nets[i], sizeof(nets[i]));
	memmove(&nets[i], &nets[i + 1], (count - (size_t)i - 1) * sizeof(nets[0]));
	save_networks(nets, count - 1);
	net_wipe(nets, sizeof(nets));
	memset(&req, 0, sizeof(req));
	req.ssid.len = (uint8_t)len;
	memcpy(req.ssid.bytes, ssid, len);
	if (drv(PIE80211_CMD_JOIN_DEL, &req, sizeof(req), 1) != 0)
		warn("%s", wname());
	close(lock);
	return 0;
}

static int
cmd_list(void)
{
	static const char *const kinds[] = { "open", "wpa2-psk", "wpa2-psk" };
	struct net nets[NET_MAX];
	size_t count, i;

	count = load_networks(nets);
	for (i = 0; i < count; i++) {
		print_ssid(nets[i].ssid, nets[i].ssid_len);
		printf("\t%s\t%d\n", kinds[nets[i].kind], nets[i].priority);
	}
	net_wipe(nets, sizeof(nets));
	return 0;
}

static int
cmd_scan(void)
{
	static const char *const secs[] = { "open", "wpa2-psk", "other" };
	struct pie80211_nodes *nodes;
	struct pie80211_node_info *ni;
	uint32_t i;

	if (drv(PIE80211_CMD_SCAN, NULL, 0, 1) == 0)
		sleep(4);
	else if (errno == EISCONN)
		warnx("%s is joined to a network and does not scan; the networks it saw before:",
		    wname());
	else
		err(1, "%s", wname());
	nodes = calloc(1, sizeof(*nodes));
	if (nodes == NULL)
		err(1, NULL);
	if (drv(PIE80211_CMD_NODES, nodes, sizeof(*nodes), 0) != 0)
		err(1, "%s", wname());
	for (i = 0; i < nodes->count && i < PIE80211_NODES_MAX; i++) {
		ni = &nodes->nodes[i];
		printf("%02x:%02x:%02x:%02x:%02x:%02x\t%u\t%d\t%s\t", ni->bssid[0], ni->bssid[1],
		    ni->bssid[2], ni->bssid[3], ni->bssid[4], ni->bssid[5], ni->channel, ni->rssi,
		    ni->security <= PIE80211_SEC_OTHER ? secs[ni->security] : "other");
		print_ssid(ni->ssid.bytes, ni->ssid.len < PIE80211_SSID_MAX ? ni->ssid.len :
		    PIE80211_SSID_MAX);
		putchar('\n');
	}
	free(nodes);
	return 0;
}

static int
get_status(struct pie80211_status *st)
{
	memset(st, 0, sizeof(*st));
	return drv(PIE80211_CMD_STATUS, st, sizeof(*st), 0);
}

static int
cmd_status(void)
{
	static const char *const states[] = { "init", "scan", "auth", "assoc", "run" };
	static const char *const problems[] = {
		[PIE80211_PROBLEM_GROUP_CIPHER] = "the network's group cipher is not supported",
		[PIE80211_PROBLEM_PAIRWISE_CIPHER] = "the network's pairwise cipher is not supported",
		[PIE80211_PROBLEM_KEY_INSTALL] = "the driver could not install the key",
		[PIE80211_PROBLEM_EAPOL_NOACK] = "the access point did not acknowledge a handshake frame",
		[PIE80211_PROBLEM_HANDSHAKE] = "the network ended the WPA2 handshake: the passphrase may be wrong",
	};
	struct pie80211_status st;

	if (get_status(&st) != 0)
		err(1, "%s", wname());
	printf("state\t%s\nlink\t%s\n", st.state <= PIE80211_STATE_RUN ? states[st.state] : "?",
	    st.link_up ? "up" : "down");
	if (st.problem != PIE80211_PROBLEM_NONE) {
		printf("problem\t");
		print_ssid(st.problem_ssid.bytes, st.problem_ssid.len < PIE80211_SSID_MAX ?
		    st.problem_ssid.len : PIE80211_SSID_MAX);
		printf(": %s\n", st.problem < sizeof(problems) / sizeof(problems[0]) ?
		    problems[st.problem] : "an unknown problem");
	}
	if (st.state != PIE80211_STATE_RUN)
		return 0;
	printf("ssid\t");
	print_ssid(st.ssid.bytes, st.ssid.len < PIE80211_SSID_MAX ? st.ssid.len : PIE80211_SSID_MAX);
	printf("\nbssid\t%02x:%02x:%02x:%02x:%02x:%02x\nchannel\t%u\nrssi\t%d\nrate\t%u kb/s\n",
	    st.bssid[0], st.bssid[1], st.bssid[2], st.bssid[3], st.bssid[4], st.bssid[5],
	    st.channel, st.rssi, st.rate_kbps);
	return 0;
}

static int
send_country(const char *code)
{
	struct pie80211_country c;

	memcpy(c.code, code, 2);
	return drv(PIE80211_CMD_COUNTRY, &c, sizeof(c), 1);
}

static int
cmd_country(const char *code)
{
	char line[3];
	int lock;

	if (strlen(code) != 2 || !isupper((unsigned char)code[0]) || !isupper((unsigned char)code[1]))
		errx(1, "a country is two capital letters");
	memcpy(line, code, 2);
	line[2] = '\n';
	lock = lock_networks();
	if (net_write_text(COUNTRY, line, sizeof(line)) != 0)
		err(1, "%s", COUNTRY);
	if (send_country(code) != 0)
		warn("%s", wname());
	close(lock);
	return 0;
}

struct load {
	size_t loaded;
	size_t refused;
	size_t badline;
	int country_error;
	int country_file;
	int file_error;
	int if_error;
	int push_error;
};

/*
 * *infile says whose an error is: COUNTRY_FILE's, COUNTRY_LOCK's, or the
 * interface's for 0. The networks lock is held from the read to the
 * kernel's update, as country holds it, so that an older country is never
 * given after a newer one.
 */
static int
load_country(int *infile)
{
	char code[4], *line;
	FILE *f;
	int error, lock;

	*infile = COUNTRY_LOCK;
	lock = net_lock(NETWORKS);
	if (lock < 0)
		return errno;
	*infile = COUNTRY_FILE;
	f = fopen(COUNTRY, "r");
	if (f == NULL) {
		error = errno == ENOENT ? 0 : errno;
		close(lock);
		return error;
	}
	errno = 0;
	line = fgets(code, sizeof(code), f);
	if (ferror(f)) {
		error = errno != 0 ? errno : EIO;
	} else if (line == NULL || !isupper((unsigned char)code[0]) ||
	    !isupper((unsigned char)code[1]) || (code[2] != '\n' && code[2] != '\0')) {
		error = EINVAL;
	} else {
		*infile = 0;
		error = send_country(code) != 0 ? errno : 0;
	}
	fclose(f);
	close(lock);
	return error;
}

static void
country_said(char *buf, size_t len, int error, int infile)
{
	if (infile == COUNTRY_LOCK)
		snprintf(buf, len, "%s.lock: %s", NETWORKS, strerror(error));
	else if (infile && error == EINVAL)
		snprintf(buf, len, "%s: not two capital letters", COUNTRY);
	else if (infile)
		snprintf(buf, len, "%s: %s", COUNTRY, strerror(error));
	else
		snprintf(buf, len, "%s: country: %s", wname(), strerror(error));
}

static void
load_networks_into(struct load *r)
{
	struct net nets[NET_MAX];
	size_t count = 0, i;
	int lock;

	lock = net_lock(NETWORKS);
	if (lock < 0) {
		r->file_error = errno;
		return;
	}
	if (net_read(NETWORKS, nets, NET_MAX, &count, &r->badline) != 0) {
		r->file_error = r->badline != 0 ? EINVAL : errno;
	} else if (drv(PIE80211_CMD_JOIN_FLUSH, NULL, 0, 1) != 0) {
		r->if_error = errno;
	} else {
		for (i = 0; i < count; i++) {
			if (push(&nets[i]) == 0) {
				r->loaded++;
			} else {
				r->refused++;
				r->push_error = errno;
			}
		}
	}
	net_wipe(nets, sizeof(nets));
	close(lock);
}

static void
load_into(struct load *r)
{
	memset(r, 0, sizeof(*r));
	r->country_error = load_country(&r->country_file);
	load_networks_into(r);
}

static int
cmd_load(void)
{
	struct load r;
	char msg[160];

	load_into(&r);
	if (r.country_error != 0) {
		country_said(msg, sizeof(msg), r.country_error, r.country_file);
		warnx("%s", msg);
	}
	if (r.badline != 0)
		errx(1, "%s: line %zu is not a network", NETWORKS, r.badline);
	if (r.file_error != 0)
		errc(1, r.file_error, "%s", NETWORKS);
	if (r.if_error != 0)
		errc(1, r.if_error, "%s", wname());
	if (r.refused != 0)
		errc(1, r.push_error, "%s: %zu of %zu networks refused", wname(), r.refused,
		    r.refused + r.loaded);
	return 0;
}

static int
kev_socket(void)
{
	struct kev_request req = { KEV_VENDOR_APPLE, KEV_NETWORK_CLASS, KEV_DL_SUBCLASS };
	int s = socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT);

	if (s >= 0 && (ioctl(s, SIOCSKEVFILT, &req) != 0 || fcntl(s, F_SETFL, O_NONBLOCK) != 0)) {
		close(s);
		s = -1;
	}
	return s;
}

/*
 * An interface watch has seen, and what it has given it: the country, and
 * all the stored networks. watch tries the rest again at each look, and
 * logs a failure once until it changes.
 */
struct watched {
	char name[IFNAMSIZ];
	int done;
	char said[2][160];
};

static void
kev_read(int s, struct watched w[WATCH_MAX])
{
	union {
		struct kern_event_msg m;
		uint8_t buf[1024];
	} in;
	const struct net_event_data *ev;
	char name[IFNAMSIZ + 16];
	ssize_t n;
	int i;

	while ((n = recv(s, &in, sizeof(in), 0)) > 0) {
		if (n < (ssize_t)(KEV_MSG_HEADER_SIZE + sizeof(*ev)) ||
		    in.m.vendor_code != KEV_VENDOR_APPLE || in.m.kev_class != KEV_NETWORK_CLASS ||
		    in.m.kev_subclass != KEV_DL_SUBCLASS || in.m.event_code != KEV_DL_IF_ATTACHED)
			continue;
		ev = (const struct net_event_data *)(const void *)in.m.event_data;
		snprintf(name, sizeof(name), "%.*s%u", IFNAMSIZ, ev->if_name, ev->if_unit);
		for (i = 0; i < WATCH_MAX; i++)
			if (strcmp(w[i].name, name) == 0)
				memset(&w[i], 0, sizeof(w[i]));
	}
}

static void
watch_say(struct watched *w, int what, int prio, const char *fmt, ...)
{
	char msg[sizeof(w->said[0])];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (strcmp(msg, w->said[what]) == 0)
		return;
	strlcpy(w->said[what], msg, sizeof(w->said[what]));
	syslog(prio, "%s", msg);
}

static void
watch_load(struct watched *w)
{
	struct load r;
	char msg[sizeof(w->said[0])];
	int error, infile;

	ifname = w->name;
	if ((w->done & WATCH_COUNTRY) == 0) {
		error = load_country(&infile);
		if (error == 0) {
			w->done |= WATCH_COUNTRY;
		} else {
			country_said(msg, sizeof(msg), error, infile);
			watch_say(w, 0, LOG_WARNING, "%s", msg);
		}
	}
	if ((w->done & WATCH_NETWORKS) == 0) {
		memset(&r, 0, sizeof(r));
		load_networks_into(&r);
		if (r.badline != 0)
			watch_say(w, 1, LOG_ERR, "%s: line %zu is not a network", NETWORKS, r.badline);
		else if (r.file_error != 0)
			watch_say(w, 1, LOG_ERR, "%s: %s", NETWORKS, strerror(r.file_error));
		else if (r.if_error != 0)
			watch_say(w, 1, LOG_ERR, "%s: %s", w->name, strerror(r.if_error));
		else if (r.refused != 0)
			watch_say(w, 1, LOG_ERR, "%s: %zu of %zu networks refused: %s", w->name,
			    r.refused, r.refused + r.loaded, strerror(r.push_error));
		else {
			w->done |= WATCH_NETWORKS;
			syslog(LOG_INFO, "%s: %zu networks loaded", w->name, r.loaded);
		}
	}
	ifname = NULL;
}

static void
watch_scan(struct watched w[WATCH_MAX])
{
	struct if_nameindex *ifs, *p;
	int i;

	ifs = if_nameindex();
	if (ifs == NULL)
		return;
	for (i = 0; i < WATCH_MAX; i++) {
		for (p = ifs; p->if_index != 0 && strcmp(p->if_name, w[i].name) != 0; p++)
			;
		if (p->if_index == 0)
			memset(&w[i], 0, sizeof(w[i]));
	}
	for (p = ifs; p->if_index != 0; p++) {
		for (i = 0; i < WATCH_MAX && strcmp(w[i].name, p->if_name) != 0; i++)
			;
		if (i == WATCH_MAX) {
			if (!is_wifi(p->if_name))
				continue;
			for (i = 0; i < WATCH_MAX && w[i].name[0] != '\0'; i++)
				;
			if (i == WATCH_MAX)
				continue;
			strlcpy(w[i].name, p->if_name, sizeof(w[i].name));
		}
		if (w[i].done != (WATCH_COUNTRY | WATCH_NETWORKS))
			watch_load(&w[i]);
	}
	if_freenameindex(ifs);
}

static int
cmd_watch(void)
{
	struct watched w[WATCH_MAX];
	struct pollfd pfd;
	int lock, kev;

	signal(SIGHUP, SIG_IGN);
	lock = open(WATCH_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lock < 0)
		err(1, "%s", WATCH_LOCK);
	if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
		if (errno == EWOULDBLOCK)
			errx(0, "watch runs already");
		err(1, "%s", WATCH_LOCK);
	}
	kev = kev_socket();
	if (kev < 0)
		err(1, "kernel event socket");
	openlog("pie-wifi", LOG_PID, LOG_DAEMON);
	memset(w, 0, sizeof(w));
	pfd.fd = kev;
	pfd.events = POLLIN;
	for (;;) {
		kev_read(kev, w);
		watch_scan(w);
		if (poll(&pfd, 1, 60000) > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			sleep(60);
	}
}

static int
remove_setup(int fd, const char *path)
{
	struct stat sb;

	if (fstat(fd, &sb) == 0 && (sb.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) != 0)
		(void)fchflags(fd, sb.st_flags & ~(UF_IMMUTABLE | SF_IMMUTABLE));
	close(fd);
	return unlink(path);
}

static int
cmd_import(const char *path)
{
	struct net nets[NET_MAX], found[NET_MAX];
	struct stat sb;
	size_t count, n, i;
	char *text;
	ssize_t got;
	int fd, lock;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(1, "%s", path);
	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > 4096)
		errx(1, "%s is not a setup file", path);
	text = calloc(1, (size_t)sb.st_size + 1);
	if (text == NULL)
		err(1, NULL);
	got = read(fd, text, (size_t)sb.st_size);
	if (got < 0)
		err(1, "%s", path);
	if (got != sb.st_size || net_import_parse(text, (size_t)got, found, NET_MAX, &n) != 0) {
		net_wipe(text, (size_t)sb.st_size);
		net_wipe(found, sizeof(found));
		errx(1, "%s: not ssid= and passphrase= lines", path);
	}
	net_wipe(text, (size_t)sb.st_size);
	free(text);
	lock = lock_networks();
	count = load_networks(nets);
	for (i = 0; i < n; i++) {
		to_psk(&found[i]);
		put(nets, &count, &found[i]);
	}
	save_networks(nets, count);
	close(lock);
	net_wipe(nets, sizeof(nets));
	net_wipe(found, sizeof(found));
	printf("%zu networks stored\n", n);
	fflush(stdout);
	if (remove_setup(fd, path) != 0) {
		warn("%s: not deleted", path);
		return IMPORT_KEPT;
	}
	return 0;
}

static int
cmd_wait(const char *arg)
{
	struct pie80211_status st;
	struct timespec tick = { 0, 250000000 };
	char *end;
	long secs = strtol(arg, &end, 10);
	long ticks;

	if (*end != '\0' || secs < 0 || secs > 3600)
		errx(1, "wait takes 0 to 3600 seconds");
	for (ticks = secs * 4; ; ticks--) {
		if (get_status(&st) == 0 && st.link_up)
			return 0;
		if (ticks <= 0)
			return 1;
		nanosleep(&tick, NULL);
	}
}

int
main(int argc, char *argv[])
{
	struct rlimit core = { 0, 0 };
	int ch;

	if (setrlimit(RLIMIT_CORE, &core) != 0)
		err(1, "core limit");
	while ((ch = getopt(argc, argv, "i:")) != -1) {
		if (ch != 'i')
			usage();
		ifname = optarg;
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || (ifname != NULL && strlen(ifname) >= IFNAMSIZ))
		usage();
	if (strcmp(argv[0], "scan") == 0 && argc == 1)
		return cmd_scan();
	if (strcmp(argv[0], "join") == 0 && argc == 2)
		return cmd_join(argv[1], 0);
	if (strcmp(argv[0], "join") == 0 && argc == 3 && strcmp(argv[2], "--open") == 0)
		return cmd_join(argv[1], 1);
	if (strcmp(argv[0], "forget") == 0 && argc == 2)
		return cmd_forget(argv[1]);
	if (strcmp(argv[0], "list") == 0 && argc == 1)
		return cmd_list();
	if (strcmp(argv[0], "status") == 0 && argc == 1)
		return cmd_status();
	if (strcmp(argv[0], "country") == 0 && argc == 2)
		return cmd_country(argv[1]);
	if (strcmp(argv[0], "load") == 0 && argc == 1)
		return cmd_load();
	if (strcmp(argv[0], "import") == 0 && argc == 2)
		return cmd_import(argv[1]);
	if (strcmp(argv[0], "wait") == 0 && argc == 2)
		return cmd_wait(argv[1]);
	if (strcmp(argv[0], "watch") == 0 && argc == 1 && ifname == NULL)
		return cmd_watch();
	usage();
	return 2;
}
