/*
 *  __MODULE__	classify.c
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Abstract:	Decide which configuration rule applies to a network interface,
 *		from the sysfs device behind it: driver, modalias and the
 *		physical path (host controller, SPI bus, USB port). The interface
 *		name is never used for matching, so renames do not matter.
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "napi_mac.h"

struct devinfo {
	char	dev[PATH_MAX];		/* resolved sysfs device path */
	char	drv[NAME_LEN];		/* bound driver name, "" if none */
	char	alias[256];		/* modalias, "" if none */
};

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* True if name is one whole component of path. */
static int has_component(const char *path, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = path; *p; ) {
		while (*p == '/')
			p++;
		const char *e = strchr(p, '/');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == n && strncmp(p, name, n) == 0)
			return 1;
		p += len;
	}
	return 0;
}

static int has_any_controller(const char *path, const struct rule *r)
{
	for (int i = 0; i < r->ncontrollers; i++)
		if (has_component(path, r->controllers[i]))
			return 1;
	return 0;
}

/*
 * Resolve the sysfs device behind a network interface.
 *
 * Returns	1 and fills di for a physical interface; 0 for virtual
 *		interfaces (bridge, vlan, tun, ...) without a backing device
 */
static int device_info(const char *iface, struct devinfo *di)
{
	char link[PATH_MAX], tmp[PATH_MAX];
	struct stat st;

	memset(di, 0, sizeof *di);
	if (snprintf(link, sizeof link, "%s/%s/device", g_paths.sys_class_net, iface) >= (int)sizeof link)
		return 0;
	if (stat(link, &st) < 0 || !realpath(link, di->dev))
		return 0;
	if (snprintf(link, sizeof link, "%s/driver", di->dev) < (int)sizeof link && realpath(link, tmp)) {
		const char *drv = base_name(tmp);
		if (strlen(drv) < sizeof di->drv)	/* longer names match no rule anyway */
			strcpy(di->drv, drv);
	}
	if (snprintf(link, sizeof link, "%s/modalias", di->dev) < (int)sizeof link &&
	    sysfs_read_line(link, di->alias, sizeof di->alias) < 0)
		di->alias[0] = '\0';
	return 1;
}

/*
 * Check that a USB network interface sits directly on the given port path.
 *
 * a_dev	sysfs path of the USB *interface* (.../usb1/1-1/1-1.1/1-1.1:1.0)
 * a_r		rule with controllers and port ("1.1")
 * Returns	1 on match
 *
 * Only the interface's own parent (the USB device directory, "1-1.1") is
 * compared, so an adapter plugged into an external hub on that port
 * ("1-1.1.3") does not match.
 */
int usb_match(const char *dev, const struct rule *r)
{
	char parent[PATH_MAX];

	if (!has_any_controller(dev, r))
		return 0;
	snprintf(parent, sizeof parent, "%s", dev);
	char *slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	const char *usbdev = base_name(parent);		/* "1-1.1" */
	const char *dash = strchr(usbdev, '-');
	return dash && strcmp(dash + 1, r->port) == 0;
}

/*
 * Decide which rule applies to an interface.
 *
 * Returns	the rule, or NULL if the interface is not managed
 */
const struct rule *classify(const char *iface, const struct config *cfg)
{
	struct devinfo di;

	if (!device_info(iface, &di))
		return NULL;

	if (strcmp(di.drv, "rk_gmac-dwmac") == 0 || strstr(di.alias, "rockchip,rk3308-gmac")) {
		for (int i = 0; i < cfg->nrules; i++)
			if (cfg->rules[i].kind == RULE_NATIVE)
				return &cfg->rules[i];
		return NULL;
	}

	/*
	 * W5500 on SPI, matched by modalias/driver and SPI bus name; optional
	 * controllers additionally pin the SPI host controller.
	 */
	if (strcmp(di.alias, "spi:w5500") == 0 || strcmp(di.drv, "w5100") == 0 || strcmp(di.drv, "w5100-spi") == 0) {
		const char *name = base_name(di.dev);
		for (int i = 0; i < cfg->nrules; i++) {
			const struct rule *r = &cfg->rules[i];
			size_t n = strlen(r->bus);
			if (r->kind != RULE_W5500)
				continue;
			if (r->ncontrollers && !has_any_controller(di.dev, r))
				continue;
			if (has_component(di.dev, r->bus) || (strncmp(name, r->bus, n) == 0 && name[n] == '.'))
				return r;
		}
		return NULL;
	}

	for (int i = 0; i < cfg->nrules; i++) {
		const struct rule *r = &cfg->rules[i];
		if (r->kind == RULE_USB && strcmp(di.drv, r->driver) == 0 && usb_match(di.dev, r))
			return r;
	}
	return NULL;
}
