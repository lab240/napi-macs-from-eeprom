/*
 *  __MODULE__	netif.c
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Abstract:	Read interface state from sysfs and change it with ioctl(2):
 *		SIOCGIFFLAGS/SIOCSIFFLAGS for down/up, SIOCSIFHWADDR for the
 *		address. Replaces the ip(8) calls of the Python helper, so the
 *		program has no runtime dependency besides libc.
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "napi_mac.h"

/*
 * Read the first line of a small file, trailing whitespace removed.
 *
 * Returns	0, or -1 with errno set
 */
int sysfs_read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f)) {
		int e = ferror(f) ? errno : EIO;
		fclose(f);
		errno = e;
		return -1;
	}
	fclose(f);
	size_t n = strlen(buf);
	while (n && isspace((unsigned char)buf[n - 1]))
		buf[--n] = '\0';
	return 0;
}

static int iface_file(const char *iface, const char *name, char *buf, size_t len, char *err, size_t errlen)
{
	char path[PATH_MAX];

	snprintf(path, sizeof path, "%s/%s/%s", g_paths.sys_class_net, iface, name);
	if (sysfs_read_line(path, buf, len) < 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

int current_mac(const char *iface, char out[MAC_STR_LEN], char *err, size_t errlen)
{
	char buf[64];

	if (iface_file(iface, "address", buf, sizeof buf, err, errlen) < 0)
		return -1;
	for (char *p = buf; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	/* A longer (non-Ethernet) address must not be truncated into a false match. */
	if (strlen(buf) >= MAC_STR_LEN) {
		snprintf(err, errlen, "%s: address '%s' is not an Ethernet MAC", iface, buf);
		return -1;
	}
	strcpy(out, buf);
	return 0;
}

static int is_up(const char *iface, int *up, char *err, size_t errlen)
{
	char buf[32];

	if (iface_file(iface, "flags", buf, sizeof buf, err, errlen) < 0)
		return -1;
	*up = (strtoul(buf, NULL, 16) & IFF_UP) != 0;
	return 0;
}

static int ifreq_open(const char *iface, struct ifreq *ifr, char *err, size_t errlen)
{
	memset(ifr, 0, sizeof *ifr);
	if (strlen(iface) >= sizeof ifr->ifr_name) {
		snprintf(err, errlen, "interface name too long");
		return -1;
	}
	strcpy(ifr->ifr_name, iface);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		snprintf(err, errlen, "socket: %s", strerror(errno));
	return fd;
}

static int real_set_up(const char *iface, int up, char *err, size_t errlen)
{
	struct ifreq ifr;
	int fd = ifreq_open(iface, &ifr, err, errlen);
	if (fd < 0)
		return -1;
	int rc = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (rc == 0) {
		if (up)
			ifr.ifr_flags |= IFF_UP;
		else
			ifr.ifr_flags &= ~IFF_UP;
		rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
	}
	if (rc < 0)
		snprintf(err, errlen, "%s", strerror(errno));
	close(fd);
	return rc < 0 ? -1 : 0;
}

static int real_set_mac(const char *iface, const char *mac, char *err, size_t errlen)
{
	unsigned int o[MAC_LEN];
	char tail;
	struct ifreq ifr;

	if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x%c", &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &tail) != MAC_LEN) {
		snprintf(err, errlen, "bad MAC '%s'", mac);
		return -1;
	}
	int fd = ifreq_open(iface, &ifr, err, errlen);
	if (fd < 0)
		return -1;
	ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	for (int i = 0; i < MAC_LEN; i++)
		ifr.ifr_hwaddr.sa_data[i] = (char)o[i];
	int rc = ioctl(fd, SIOCSIFHWADDR, &ifr);
	if (rc < 0)
		snprintf(err, errlen, "%s", strerror(errno));
	close(fd);
	return rc < 0 ? -1 : 0;
}

const struct netops real_netops = {
	.set_up		= real_set_up,
	.set_mac	= real_set_mac,
	.indextoname	= if_indextoname,
};
const struct netops *g_netops = &real_netops;

/*
 * Set the MAC address on an interface, cycling it down/up only if it was up.
 *
 * a_target	desired MAC, lowercase
 * a_result	"ALREADY" if nothing changed, "OK" if the address was set
 * Returns	0, or -1 with a message in err that states the resulting
 *		interface state
 */
int apply_mac(const char *iface, const char *target, const char **result, char *err, size_t errlen)
{
	char cur[MAC_STR_LEN], e1[MSG_MAX], e2[MSG_MAX];
	int was_up;

	if (current_mac(iface, cur, err, errlen) < 0)
		return -1;
	if (strcmp(cur, target) == 0) {
		*result = "ALREADY";
		return 0;
	}
	if (is_up(iface, &was_up, err, errlen) < 0)	/* always down in udev "add" context */
		return -1;
	if (was_up && g_netops->set_up(iface, 0, e1, sizeof e1) < 0) {
		snprintf(err, errlen, "link down failed: %s; MAC unchanged", e1);
		return -1;
	}
	if (g_netops->set_mac(iface, target, e1, sizeof e1) < 0) {
		if (was_up && g_netops->set_up(iface, 1, e2, sizeof e2) < 0)
			snprintf(err, errlen, "setting address failed: %s; MAC unchanged; interface left DOWN (%s)", e1, e2);
		else
			snprintf(err, errlen, "setting address failed: %s; MAC unchanged", e1);
		return -1;
	}
	if (was_up && g_netops->set_up(iface, 1, e1, sizeof e1) < 0) {
		snprintf(err, errlen, "MAC set to %s, but interface left DOWN: %s", target, e1);
		return -1;
	}
	*result = "OK";
	return 0;
}
