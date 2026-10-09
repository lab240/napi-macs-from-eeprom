/*
 *  __MODULE__	napi_mac.c
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Abstract:	Driver logic of napi-set-mac: handle one interface, pass over
 *		all interfaces, "--check" report, command line and logging.
 *
 *  Usage:	napi-set-mac <ifname>		handle one interface
 *		napi-set-mac --ifindex <n>	handle one interface by index (udev)
 *		napi-set-mac --all		pass over all existing interfaces
 *		napi-set-mac --check		validate config and EEPROM, show
 *						the plan; changes nothing
 *
 *  Exit codes:	0  MAC set, or already correct (--all/--check: no errors)
 *		1  error (EEPROM invalid, interface operation failed, config error)
 *		3  interface not matched by configuration (nothing to do)
 *		4  interface matched, but the EEPROM is not available yet; it is
 *		   handled by the "--all" pass started when the EEPROM appears
 *		Under udev, 3 and 4 are reported as 0 (see main.c).
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "napi_mac.h"

struct napi_paths g_paths = {
	.eeprom		= "/sys/bus/i2c/devices/1-0050/eeprom",
	.config		= "/etc/napi/mac.conf",
	.sys_class_net	= "/sys/class/net",
};

int g_in_udev;

/*
 * Write a log line: syslog under udev (stdout is discarded there),
 * stdout/stderr otherwise.
 */
void logmsg(int priority, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (g_in_udev) {
		vsyslog(priority, fmt, ap);
	} else {
		FILE *f = priority <= LOG_WARNING ? stderr : stdout;
		fputs("napi-set-mac: ", f);
		vfprintf(f, fmt, ap);
		fputc('\n', f);
		fflush(f);
	}
	va_end(ap);
}

static int eeprom_get(struct eeprom_cache *c, const uint8_t **b)
{
	if (!c->loaded) {
		c->status = eeprom_read(g_paths.eeprom, c->b, c->err, sizeof c->err);
		c->loaded = 1;
	}
	*b = c->b;
	return c->status;
}

/*
 * Assign the configured MAC to one interface.
 *
 * a_cache	EEPROM image read on first use, so that "--all" reads the chip
 *		once and only if some interface matches
 * Returns	exit code
 */
int handle(const char *iface, const struct config *cfg, struct eeprom_cache *cache)
{
	char mac[MAC_STR_LEN], err[MSG_MAX];
	const uint8_t *b;
	const char *result;

	if (strchr(iface, '/') || strcmp(iface, ".") == 0 || strcmp(iface, "..") == 0)
		return EXIT_NOMATCH;
	const struct rule *r = classify(iface, cfg);
	if (!r)
		return EXIT_NOMATCH;
	switch (eeprom_get(cache, &b)) {
	case EE_UNAVAILABLE:
		logmsg(LOG_INFO, "%s: %s: %s, deferred until it appears", iface, r->section, cache->err);
		return EXIT_DEFERRED;
	case EE_INVALID:
		logmsg(LOG_ERR, "%s: %s: %s; MAC unchanged", iface, r->section, cache->err);
		return EXIT_ERROR;
	}
	if (eeprom_get_mac(b, r->slot, mac, err, sizeof err) < 0) {
		logmsg(LOG_ERR, "%s: %s: %s; MAC unchanged", iface, r->section, err);
		return EXIT_ERROR;
	}
	if (apply_mac(iface, mac, &result, err, sizeof err) < 0) {
		logmsg(LOG_ERR, "%s: %s MAC%d %s: %s", iface, r->section, r->slot, mac, err);
		return EXIT_ERROR;
	}
	logmsg(LOG_INFO, "%s: %s MAC%d %s %s", iface, r->section, r->slot, mac, result);
	return EXIT_OK;
}

static int skip_lo(const struct dirent *d)
{
	return d->d_name[0] != '.' && strcmp(d->d_name, "lo") != 0;
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

/* Interfaces sorted by name, "lo" excluded. Returns count or -1. */
static int list_ifaces(struct dirent ***list)
{
	return scandir(g_paths.sys_class_net, list, skip_lo, by_name);
}

static void free_list(struct dirent **list, int n)
{
	for (int i = 0; i < n; i++)
		free(list[i]);
	free(list);
}

/*
 * Pass over all existing interfaces.
 *
 * Returns	EXIT_ERROR if any interface failed, EXIT_OK otherwise
 */
int run_all(const struct config *cfg)
{
	struct eeprom_cache cache = { 0 };
	struct dirent **list;
	int counts[5] = { 0 };

	int n = list_ifaces(&list);
	if (n < 0) {
		logmsg(LOG_ERR, "cannot scan %s: %s", g_paths.sys_class_net, strerror(errno));
		return EXIT_ERROR;
	}
	for (int i = 0; i < n; i++)
		counts[handle(list[i]->d_name, cfg, &cache)]++;
	free_list(list, n);

	logmsg(counts[EXIT_ERROR] || counts[EXIT_DEFERRED] ? LOG_WARNING : LOG_INFO,
	       "pass complete: set/already=%d not-managed=%d deferred=%d errors=%d",
	       counts[EXIT_OK], counts[EXIT_NOMATCH], counts[EXIT_DEFERRED], counts[EXIT_ERROR]);
	return counts[EXIT_ERROR] ? EXIT_ERROR : EXIT_OK;
}

/*
 * Validate configuration and EEPROM and print what would be done.
 *
 * Returns	EXIT_OK, or EXIT_ERROR if any problem was found
 */
int run_check(const struct config *cfg)
{
	uint8_t b[EEPROM_SIZE];
	char err[MSG_MAX], mac[MAC_STR_LEN];
	int ok = 1, have_eeprom = 0;
	struct dirent **list = NULL;

	for (int i = 0; i < cfg->nwarnings; i++)
		printf("WARNING %s\n", cfg->warnings[i]);
	if (!cfg->enabled)
		printf("[mac] enabled = false: nothing is managed\n");
	if (eeprom_read(g_paths.eeprom, b, err, sizeof err) == EE_OK) {
		have_eeprom = 1;
		printf("EEPROM %s: v%u platform %u mac_count %u CRC OK\n",
		       g_paths.eeprom, b[VER_OFF], b[PLATFORM_OFF], b[MAC_COUNT_OFF]);
	} else {
		printf("ERROR %s\n", err);
		ok = 0;
	}

	int n = list_ifaces(&list);
	if (n < 0) {
		printf("WARNING cannot scan %s: %s\n", g_paths.sys_class_net, strerror(errno));
		n = 0;
	}
	for (int i = 0; i < cfg->nrules; i++) {
		const struct rule *r = &cfg->rules[i];
		printf("[%s] MAC%d", r->section, r->slot);
		if (have_eeprom) {
			if (eeprom_get_mac(b, r->slot, mac, err, sizeof err) == 0) {
				printf(" %s", mac);
			} else {
				printf(" ERROR %s", err);
				ok = 0;
			}
		}
		int hits = 0;
		for (int k = 0; k < n; k++) {
			if (classify(list[k]->d_name, cfg) == r)
				printf("%s%s", hits++ ? ", " : " -> ", list[k]->d_name);
		}
		if (!hits)
			printf(" -> (no interface present)");
		if (hits > 1) {
			printf(" ERROR several interfaces match");
			ok = 0;
		}
		printf("\n");
	}
	if (list)
		free_list(list, n);
	fflush(stdout);
	return ok ? EXIT_OK : EXIT_ERROR;
}

static void usage(FILE *f)
{
	fprintf(f, "usage: napi-set-mac <ifname> | --ifindex <n> | --all | --check | --version\n");
}

int napi_main(int argc, char **argv)
{
	char namebuf[IF_NAMESIZE], err[MSG_MAX];
	const char *arg;
	struct config cfg;

	g_in_udev = getenv("ACTION") && getenv("SUBSYSTEM");
	if (g_in_udev)
		openlog("napi-set-mac", LOG_PID, LOG_DAEMON);

	if (argc == 3 && strcmp(argv[1], "--ifindex") == 0) {
		/*
		 * udev substitutes $name / $env{INTERFACE} in RUN when the rule
		 * is parsed, before NAME= in later rules takes effect, so a name
		 * passed on the command line can already be stale. The index
		 * survives renames.
		 */
		char *end;
		errno = 0;
		unsigned long idx = strtoul(argv[2], &end, 10);
		if (errno || end == argv[2] || *end || idx == 0 || idx > 0x7FFFFFFF) {
			logmsg(LOG_ERR, "bad ifindex '%s'; MAC unchanged", argv[2]);
			return EXIT_ERROR;
		}
		arg = g_netops->indextoname((unsigned)idx, namebuf);
		if (!arg) {
			logmsg(LOG_WARNING, "ifindex %s: interface gone, nothing to do", argv[2]);
			return EXIT_NOMATCH;
		}
	} else if (argc == 2) {
		arg = argv[1];
		if (strcmp(arg, "--version") == 0) {
			printf("napi-set-mac %s\n", NAPI_VERSION);
			return EXIT_OK;
		}
		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			usage(stdout);
			return EXIT_OK;
		}
	} else {
		usage(stderr);
		return EXIT_ERROR;
	}
	if (strcmp(arg, "lo") == 0)
		return EXIT_NOMATCH;

	if (config_load(g_paths.config, &cfg, err, sizeof err) < 0) {
		if (strcmp(arg, "--check") == 0)
			printf("ERROR %s\n", err);
		else
			logmsg(LOG_ERR, "%s: %s; MAC unchanged", arg, err);
		return EXIT_ERROR;
	}

	if (strcmp(arg, "--check") == 0)
		return run_check(&cfg);
	if (!cfg.enabled)
		return EXIT_NOMATCH;
	if (strcmp(arg, "--all") == 0)
		return run_all(&cfg);
	if (arg[0] == '-') {
		usage(stderr);
		return EXIT_ERROR;
	}
	struct eeprom_cache cache = { 0 };
	return handle(arg, &cfg, &cache);
}
