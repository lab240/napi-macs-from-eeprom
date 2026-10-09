/*
 *  Unit tests for napi-set-mac (C version).
 *
 *  Run:	make test
 *
 *  EEPROM parsing is checked against the reference image from section 11 of
 *  NAPI_EEPROM_SPEC.md; interface classification and MAC assignment run on a
 *  fake sysfs tree in a temporary directory, with kernel operations replaced
 *  by fakes. Ported from the Python v12.1 test suite.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "napi_mac.h"

static int g_checks, g_failures;
static const char *g_test;

#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; \
	fprintf(stderr, "FAIL %s (%s:%d): %s\n", g_test, __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); g_checks++; if (_a != _b) { g_failures++; \
	fprintf(stderr, "FAIL %s (%s:%d): %s == %ld, expected %ld\n", g_test, __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); g_checks++; if (strcmp(_a, _b) != 0) { g_failures++; \
	fprintf(stderr, "FAIL %s (%s:%d): %s == \"%s\", expected \"%s\"\n", g_test, __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK_HAS(s, sub) do { const char *_s = (s), *_t = (sub); g_checks++; if (!strstr(_s, _t)) { g_failures++; \
	fprintf(stderr, "FAIL %s (%s:%d): \"%s\" does not contain \"%s\"\n", g_test, __FILE__, __LINE__, _s, _t); } } while (0)

/* NAPI_EEPROM_SPEC.md section 11, bytes 0x00..0x7F; 0x80..0xFF are zero. */
static const uint8_t SPEC_IMAGE[256] = {
	0x4e, 0x41, 0x50, 0x49, 0x03, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x45, 0x00, 0x00,
	0x05, 0x00, 0x00, 0x00, 0x1a, 0x09, 0x11, 0x08, 0x46, 0x43, 0x43, 0x52, 0x33, 0x33, 0x30, 0x38,
	0x2d, 0x52, 0x4f, 0x55, 0x54, 0x45, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x9e, 0xe6, 0x97, 0x4d, 0x60, 0x02, 0x9e,
	0xe6, 0x97, 0x4d, 0x61, 0x02, 0x9e, 0xe6, 0x97, 0x4d, 0x62, 0x02, 0x9e, 0xe6, 0x97, 0x4d, 0x63,
	0x02, 0x9e, 0xe6, 0x97, 0x4d, 0x64, 0x02, 0x9e, 0xe6, 0x97, 0x4d, 0x65, 0x02, 0x9e, 0xe6, 0x97,
	0x4d, 0x66, 0x02, 0x9e, 0xe6, 0x97, 0x4d, 0x67, 0x01, 0x05, 0x09, 0x0b, 0x13, 0x1d, 0x04, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x0e, 0x3a, 0xe5, 0x00, 0x00,
};

#define USB_EHCI	"devices/platform/ff440000.usb/usb1/1-1"
#define USB_OTG		"devices/platform/ff400000.usb/usb2/2-1"
#define SPI2_DEV	"devices/platform/ff4b0000.spi/spi_master/spi2/spi2.0"

static char g_root[PATH_MAX], g_eeprom[PATH_MAX], g_config[PATH_MAX], g_net[PATH_MAX];

/* ---- fixture ---------------------------------------------------------- */

static void die(const char *what)
{
	perror(what);
	exit(2);
}

static void mkdir_p(const char *path)
{
	char buf[PATH_MAX];
	snprintf(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0755) < 0 && errno != EEXIST)
				die(buf);
			*p = '/';
		}
	}
	if (mkdir(buf, 0755) < 0 && errno != EEXIST)
		die(buf);
}

static void write_file(const char *path, const void *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, data, len) != (ssize_t)len)
		die(path);
	close(fd);
}

static void write_text(const char *path, const char *text)
{
	write_file(path, text, strlen(text));
}

static void rm_rf(const char *path)
{
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: cannot remove %s\n", path);
}

static void setup(const char *name)
{
	const char *tmp = getenv("TMPDIR");
	g_test = name;
	snprintf(g_root, sizeof g_root, "%s/napi-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(g_root))
		die("mkdtemp");
	snprintf(g_eeprom, sizeof g_eeprom, "%s/eeprom", g_root);
	snprintf(g_config, sizeof g_config, "%s/mac.conf", g_root);
	snprintf(g_net, sizeof g_net, "%s/sys/class/net", g_root);
	mkdir_p(g_net);
	g_paths.eeprom = g_eeprom;
	g_paths.config = g_config;
	g_paths.sys_class_net = g_net;
	g_netops = &real_netops;
}

static void teardown(void)
{
	rm_rf(g_root);
}

static void eeprom(const uint8_t *img, size_t len)
{
	write_file(g_eeprom, img, len);
}

static void config(const char *text)
{
	write_text(g_config, text);
}

/* Create sys/class/net/<name> pointing to sys/<devpath>. */
static void iface(const char *name, const char *devpath, const char *driver, const char *modalias, int up)
{
	char dev[PATH_MAX], p[PATH_MAX], net[PATH_MAX];

	snprintf(dev, sizeof dev, "%s/sys/%s", g_root, devpath);
	mkdir_p(dev);
	if (driver) {
		snprintf(p, sizeof p, "%s/sys/bus/drivers/%s", g_root, driver);
		mkdir_p(p);
		char link[PATH_MAX + 8];
		snprintf(link, sizeof link, "%s/driver", dev);
		if (symlink(p, link) < 0 && errno != EEXIST)
			die(link);
	}
	if (modalias) {
		snprintf(p, sizeof p, "%s/modalias", dev);
		char line[256];
		snprintf(line, sizeof line, "%s\n", modalias);
		write_text(p, line);
	}
	snprintf(net, sizeof net, "%s/%s", g_net, name);
	mkdir_p(net);
	snprintf(p, sizeof p, "%s/device", net);
	if (symlink(dev, p) < 0)
		die(p);
	snprintf(p, sizeof p, "%s/address", net);
	write_text(p, "aa:bb:cc:dd:ee:01\n");
	snprintf(p, sizeof p, "%s/flags", net);
	write_text(p, up ? "0x1003\n" : "0x1002\n");
}

static void virtual_iface(const char *name)
{
	char net[PATH_MAX], p[PATH_MAX + 16];
	snprintf(net, sizeof net, "%s/%s", g_net, name);
	mkdir_p(net);
	snprintf(p, sizeof p, "%s/address", net);
	write_text(p, "00:00:00:00:00:00\n");
	snprintf(p, sizeof p, "%s/flags", net);
	write_text(p, "0x1003\n");
}

static const char *mac_of(const char *name)
{
	static char buf[64];
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/%s/address", g_net, name);
	if (sysfs_read_line(p, buf, sizeof buf) < 0)
		return "?";
	return buf;
}

static void set_address(const char *name, const char *mac)
{
	char p[PATH_MAX], line[32];
	snprintf(p, sizeof p, "%s/%s/address", g_net, name);
	snprintf(line, sizeof line, "%s\n", mac);
	write_text(p, line);
}

/* Copy of img with byte changes and a recomputed CRC. */
static void patched(uint8_t *out, const uint8_t *img, int off, const uint8_t *data, size_t len)
{
	memcpy(out, img, 256);
	memcpy(out + off, data, len);
	uint32_t crc = crc32_iso_hdlc(out, CRC_OFF);
	for (int i = 0; i < 4; i++)
		out[CRC_OFF + i] = (uint8_t)(crc >> (8 * i));
}

static void patched_at(uint8_t *out, const uint8_t *img, int off, uint8_t val)
{
	patched(out, img, off, &val, 1);
}

/* ---- fake kernel operations ------------------------------------------ */

static char g_calls[16][64];
static int g_ncalls;
static int g_fail_up, g_fail_down, g_fail_mac;

static int fake_set_up(const char *ifn, int up, char *err, size_t errlen)
{
	snprintf(g_calls[g_ncalls++ % 16], 64, "%s %s", ifn, up ? "up" : "down");
	if (up ? g_fail_up : g_fail_down) {
		snprintf(err, errlen, "fake %s failed", up ? "up" : "down");
		return -1;
	}
	return 0;
}

static int fake_set_mac(const char *ifn, const char *mac, char *err, size_t errlen)
{
	snprintf(g_calls[g_ncalls++ % 16], 64, "%s address %s", ifn, mac);
	if (g_fail_mac) {
		snprintf(err, errlen, "fake address failed");
		return -1;
	}
	set_address(ifn, mac);
	return 0;
}

static char *fake_indextoname(unsigned idx, char *name)
{
	if (idx != 7) {
		errno = ENODEV;
		return NULL;
	}
	strcpy(name, "lanusb1");
	return name;
}

static const struct netops fake_netops = { fake_set_up, fake_set_mac, fake_indextoname };

static void use_fakes(void)
{
	g_ncalls = 0;
	g_fail_up = g_fail_down = g_fail_mac = 0;
	g_netops = &fake_netops;
}

/* ---- EEPROM ----------------------------------------------------------- */

static void test_crc_check_value(void)
{
	g_test = "crc_check_value";
	CHECK_EQ(crc32_iso_hdlc((const uint8_t *)"123456789", 9), 0xCBF43926u);
}

static void test_spec_image(void)
{
	uint8_t b[EEPROM_SIZE];
	char err[MSG_MAX], mac[MAC_STR_LEN];

	setup("spec_image");
	eeprom(SPEC_IMAGE, sizeof SPEC_IMAGE);
	CHECK_EQ(eeprom_read(g_eeprom, b, err, sizeof err), EE_OK);
	CHECK_EQ(crc32_iso_hdlc(b, CRC_OFF), 0xE53A0E41u);
	CHECK_EQ(eeprom_get_mac(b, 1, mac, err, sizeof err), 0);
	CHECK_STR(mac, "02:9e:e6:97:4d:60");
	CHECK_EQ(eeprom_get_mac(b, 8, mac, err, sizeof err), 0);
	CHECK_STR(mac, "02:9e:e6:97:4d:67");
	/* Only the v3 area is needed. */
	eeprom(SPEC_IMAGE, EEPROM_SIZE);
	CHECK_EQ(eeprom_read(g_eeprom, b, err, sizeof err), EE_OK);
	teardown();
}

static void test_eeprom_missing(void)
{
	uint8_t b[EEPROM_SIZE];
	char err[MSG_MAX];

	setup("eeprom_missing");
	/* I2C device directory present, file missing: driver not bound yet. */
	CHECK_EQ(eeprom_read(g_eeprom, b, err, sizeof err), EE_UNAVAILABLE);
	CHECK_HAS(err, "not available: at24 driver not bound yet");
	/* No I2C device at all: not in the device tree. */
	char path[PATH_MAX + 16];
	snprintf(path, sizeof path, "%s/1-0050/eeprom", g_root);
	CHECK_EQ(eeprom_read(path, b, err, sizeof err), EE_UNAVAILABLE);
	CHECK_HAS(err, "no I2C device 1-0050 (EEPROM not in the device tree, overlay i2c1-at24 not enabled?)");
	teardown();
}

static void test_eeprom_errors(void)
{
	uint8_t img[256], b[EEPROM_SIZE];
	char err[MSG_MAX];
	static const struct { const char *text; int kind; int off; uint8_t val; } cases[] = {
		{ "short read",		0, 0, 0 },
		{ "uninitialized",	1, 0, 0 },
		{ "magic",		2, 0, 'X' },
		{ "layout v2",		3, VER_OFF, 2 },
		{ "CRC invalid",	2, 0x40, 0x00 },
		{ "platform_id 2",	3, PLATFORM_OFF, 2 },
		{ "mac_count 9",	3, MAC_COUNT_OFF, 9 },
	};

	setup("eeprom_errors");
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		size_t len = 256;
		switch (cases[i].kind) {
		case 0: memcpy(img, SPEC_IMAGE, 256); len = 0x7D; break;
		case 1: memset(img, 0xFF, 256); break;
		case 2: memcpy(img, SPEC_IMAGE, 256); img[cases[i].off] = cases[i].val; break;
		case 3: patched_at(img, SPEC_IMAGE, cases[i].off, cases[i].val); break;
		}
		eeprom(img, len);
		err[0] = '\0';
		CHECK_EQ(eeprom_read(g_eeprom, b, err, sizeof err), EE_INVALID);
		CHECK_HAS(err, cases[i].text);
	}
	teardown();
}

static void test_slot_checks(void)
{
	uint8_t b[256], tmp[256];
	char err[MSG_MAX], mac[MAC_STR_LEN];
	static const uint8_t zero[6] = { 0 };
	uint8_t mcast[6];

	g_test = "slot_checks";
	patched_at(b, SPEC_IMAGE, MAC_COUNT_OFF, 3);
	CHECK_EQ(eeprom_get_mac(b, 3, mac, err, sizeof err), 0);
	CHECK_STR(mac, "02:9e:e6:97:4d:62");
	CHECK_EQ(eeprom_get_mac(b, 4, mac, err, sizeof err), -1);
	CHECK_HAS(err, "not programmed");
	CHECK_EQ(eeprom_get_mac(b, 9, mac, err, sizeof err), -1);
	CHECK_HAS(err, "out of range");

	patched(b, SPEC_IMAGE, MAC_BASE_OFF + 6, zero, 6);
	CHECK_EQ(eeprom_get_mac(b, 2, mac, err, sizeof err), -1);
	CHECK_HAS(err, "blank");

	memcpy(mcast, SPEC_IMAGE + MAC_BASE_OFF + 6, 6);
	mcast[0] = 0x03;
	patched(b, SPEC_IMAGE, MAC_BASE_OFF + 6, mcast, 6);
	CHECK_EQ(eeprom_get_mac(b, 2, mac, err, sizeof err), -1);
	CHECK_HAS(err, "multicast");

	patched(b, SPEC_IMAGE, MAC_BASE_OFF + 6, SPEC_IMAGE + MAC_BASE_OFF, 6);
	CHECK_EQ(eeprom_get_mac(b, 2, mac, err, sizeof err), -1);
	CHECK_HAS(err, "duplicates slot 1");

	/* A duplicate beyond mac_count is not a valid MAC and does not count. */
	patched(tmp, SPEC_IMAGE, MAC_BASE_OFF + 7 * 6, SPEC_IMAGE + MAC_BASE_OFF + 6, 6);
	patched_at(b, tmp, MAC_COUNT_OFF, 7);
	CHECK_EQ(eeprom_get_mac(b, 2, mac, err, sizeof err), 0);
	CHECK_STR(mac, "02:9e:e6:97:4d:61");
}

/* ---- configuration ---------------------------------------------------- */

static const char FULL_CONFIG[] =
	"# comment\n"
	"[mac]\n"
	"enabled = true\n"
	"set_native_eth_mac = false\n"
	"\n"
	"[w5500_spi1]\n"
	"enabled = false\n"
	"mac_slot = 2\n"
	"\n"
	"[w5500_spi2]\n"
	"enabled = true\n"
	"mac_slot = 3\n"
	"\n"
	"[usb_eth_1]\n"
	"enabled = true\n"
	"controller = ff440000.usb ff450000.usb\n"
	"port = 1.1\n"
	"mac_slot = 4\n"
	"\n"
	"[usb_eth_2]\n"
	"enabled = true\n"
	"controller = ff440000.usb, ff450000.usb\n"
	"port = 1.2\n"
	"mac_slot = 5\n"
	"\n"
	"[usb_eth_3]\n"
	"enabled = true\n"
	"controller = ff400000.usb\n"
	"port = 1.1\n"
	"mac_slot = 6\n";

static void test_config_full(void)
{
	struct config cfg;
	char err[MSG_MAX];

	setup("config_full");
	config(FULL_CONFIG);
	CHECK_EQ(config_load(g_config, &cfg, err, sizeof err), 0);
	CHECK_EQ(cfg.enabled, 1);
	CHECK_EQ(cfg.nwarnings, 0);
	CHECK_EQ(cfg.nrules, 4);
	static const char *const secs[] = { "w5500_spi2", "usb_eth_1", "usb_eth_2", "usb_eth_3" };
	for (int i = 0; i < 4 && i < cfg.nrules; i++) {
		CHECK_STR(cfg.rules[i].section, secs[i]);
		CHECK_EQ(cfg.rules[i].slot, 3 + i);
	}
	CHECK_EQ(cfg.rules[1].ncontrollers, 2);
	CHECK_STR(cfg.rules[1].controllers[1], "ff450000.usb");
	CHECK_EQ(cfg.rules[2].ncontrollers, 2);
	CHECK_STR(cfg.rules[2].controllers[0], "ff440000.usb");
	CHECK_STR(cfg.rules[1].driver, "smsc95xx");
	CHECK_STR(cfg.rules[0].bus, "spi2");
	teardown();
}

static void test_config_missing(void)
{
	struct config cfg;
	char err[MSG_MAX];

	setup("config_missing");
	CHECK_EQ(config_load(g_config, &cfg, err, sizeof err), 0);
	CHECK_EQ(cfg.enabled, 0);
	CHECK_EQ(cfg.nrules, 0);
	teardown();
}

static void test_config_errors(void)
{
	struct config cfg;
	char err[MSG_MAX];
	static const struct { const char *text; const char *cfg; } cases[] = {
		{ "used by both [w5500_spi2] and [usb_eth_1]",
		  "[w5500_spi2]\nenabled = true\n[usb_eth_1]\nenabled = true\ncontroller = a\nport = 1\nmac_slot = 3\n" },
		{ "used by both [native] and [usb_eth_1]",
		  "[mac]\nset_native_eth_mac = true\n[usb_eth_1]\nenabled = true\ncontroller = a\nport = 1\nmac_slot = 1\n" },
		{ "[usb_eth_1] missing required key 'port'",
		  "[usb_eth_1]\nenabled = true\ncontroller = a\nmac_slot = 4\n" },
		{ "[usb_eth_1] missing required key 'mac_slot'",
		  "[usb_eth_1]\nenabled = true\ncontroller = a\nport = 1\n" },
		{ "[usb_eth_1] missing required key 'controller'",
		  "[usb_eth_1]\nenabled = true\nport = 1\nmac_slot = 4\n" },
		{ "mac_slot 9 out of range",
		  "[w5500_spi1]\nenabled = true\nmac_slot = 9\n" },
		{ "mac_slot = 'x' is not a number",
		  "[w5500_spi1]\nenabled = true\nmac_slot = x\n" },
		{ "enabled = 'maybe' is not a boolean",
		  "[w5500_spi1]\nenabled = maybe\n" },
		{ "both match port 1.1 on ff450000.usb",
		  "[usb_eth_1]\nenabled = true\ncontroller = ff440000.usb ff450000.usb\nport = 1.1\nmac_slot = 4\n"
		  "[usb_eth_2]\nenabled = true\ncontroller = ff450000.usb\nport = 1.1\nmac_slot = 5\n" },
		{ "key outside of any [section]", "enabled = true\n" },
		{ "duplicate key 'enabled' in [mac]", "[mac]\nenabled = true\nEnabled = false\n" },
		{ "section [mac] already exists", "[mac]\n[mac]\n" },
		{ "expected 'key = value'", "[mac]\nenabled\n" },
		{ "continuation lines", "[mac]\nenabled = true\n  more\n" },
	};

	setup("config_errors");
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		config(cases[i].cfg);
		err[0] = '\0';
		CHECK_EQ(config_load(g_config, &cfg, err, sizeof err), -1);
		CHECK_HAS(err, cases[i].text);
	}
	teardown();
}

static void test_config_syntax(void)
{
	struct config cfg;
	char err[MSG_MAX];

	setup("config_syntax");
	/* "key: value", case-insensitive keys, ';' comments, indented comments. */
	config("; top\n[mac]\nENABLED: yes\n  # indented comment\n[w5500_spi1]\nEnabled = ON\nMac_Slot: 2\n");
	CHECK_EQ(config_load(g_config, &cfg, err, sizeof err), 0);
	CHECK_EQ(cfg.enabled, 1);
	CHECK_EQ(cfg.nrules, 1);
	CHECK_EQ(cfg.rules[0].slot, 2);
	teardown();
}

static void test_config_disabled_not_validated(void)
{
	struct config cfg;
	char err[MSG_MAX];

	setup("config_disabled_not_validated");
	config("[usb_eth_1]\nenabled = false\n[w5500_spi1]\nmac_slot = 3\n[w5500_spi2]\nenabled = true\n");
	CHECK_EQ(config_load(g_config, &cfg, err, sizeof err), 0);
	CHECK_EQ(cfg.nrules, 1);
	CHECK_STR(cfg.rules[0].section, "w5500_spi2");
	CHECK_EQ(cfg.rules[0].slot, 3);
	teardown();
}

static void test_config_unknown_section(void)
{
	struct config cfg;
	char err[MSG_MAX];

	setup("config_unknown_section");
	config("[usb_eth1]\nenabled = true\n");
	CHECK_EQ(config_load(g_config, &cfg, err, sizeof err), 0);
	CHECK_EQ(cfg.nwarnings, 1);
	CHECK_STR(cfg.warnings[0], "config: unknown section [usb_eth1] ignored");
	teardown();
}

/* ---- classification --------------------------------------------------- */

static const char *section_of(const char *ifn, const struct config *cfg)
{
	const struct rule *r = classify(ifn, cfg);
	return r ? r->section : "(none)";
}

static void load(struct config *cfg)
{
	char err[MSG_MAX];
	if (config_load(g_config, cfg, err, sizeof err) < 0) {
		fprintf(stderr, "config: %s\n", err);
		exit(2);
	}
}

static void test_classify_usb(void)
{
	struct config cfg;

	setup("classify_usb");
	config(FULL_CONFIG);
	load(&cfg);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	iface("eth2", USB_EHCI "/1-1.2/1-1.2:1.0", "smsc95xx", NULL, 0);
	iface("eth3", USB_OTG "/2-1.1/2-1.1:1.0", "smsc95xx", NULL, 0);
	iface("eth4", "devices/platform/ff450000.usb/usb3/3-1/3-1.1/3-1.1:1.0", "smsc95xx", NULL, 0);
	CHECK_STR(section_of("eth1", &cfg), "usb_eth_1");
	CHECK_STR(section_of("eth2", &cfg), "usb_eth_2");
	CHECK_STR(section_of("eth3", &cfg), "usb_eth_3");
	CHECK_STR(section_of("eth4", &cfg), "usb_eth_1");
	teardown();
}

static void test_classify_usb_not_matched(void)
{
	struct config cfg;

	setup("classify_usb_not_matched");
	config(FULL_CONFIG);
	load(&cfg);
	iface("hub", USB_EHCI "/1-1.1/1-1.1.3/1-1.1.3:1.0", "smsc95xx", NULL, 0);
	iface("drv", USB_EHCI "/1-1.2/1-1.2:1.0", "r8152", NULL, 0);
	iface("port", USB_EHCI "/1-1.4/1-1.4:1.0", "smsc95xx", NULL, 0);
	virtual_iface("br0");
	CHECK_STR(section_of("hub", &cfg), "(none)");
	CHECK_STR(section_of("drv", &cfg), "(none)");
	CHECK_STR(section_of("port", &cfg), "(none)");
	CHECK_STR(section_of("br0", &cfg), "(none)");
	CHECK_STR(section_of("nonexistent", &cfg), "(none)");
	teardown();
}

static void test_classify_w5500(void)
{
	struct config cfg;

	setup("classify_w5500");
	config(FULL_CONFIG);
	load(&cfg);
	iface("eth5", SPI2_DEV, "w5100", "spi:w5500", 0);
	iface("eth6", "devices/platform/ff4a0000.spi/spi_master/spi1/spi1.0", "w5100", "spi:w5500", 0);
	CHECK_STR(section_of("eth5", &cfg), "w5500_spi2");
	CHECK_STR(section_of("eth6", &cfg), "(none)");		/* w5500_spi1 disabled */

	config("[w5500_spi2]\nenabled = true\ncontroller = ff4c0000.spi\n");
	load(&cfg);
	CHECK_STR(section_of("eth5", &cfg), "(none)");
	config("[w5500_spi2]\nenabled = true\ncontroller = ff4b0000.spi\n");
	load(&cfg);
	CHECK_STR(section_of("eth5", &cfg), "w5500_spi2");
	teardown();
}

static void test_classify_native(void)
{
	struct config cfg;

	setup("classify_native");
	config(FULL_CONFIG);
	load(&cfg);
	iface("eth0", "devices/platform/ff4e0000.ethernet", "rk_gmac-dwmac", NULL, 0);
	CHECK_STR(section_of("eth0", &cfg), "(none)");
	config("[mac]\nset_native_eth_mac = true\n");
	load(&cfg);
	CHECK_STR(section_of("eth0", &cfg), "native");
	teardown();
}

/* ---- MAC assignment --------------------------------------------------- */

static void apply_setup(const char *name, struct config *cfg)
{
	setup(name);
	config(FULL_CONFIG);
	load(cfg);
	eeprom(SPEC_IMAGE, sizeof SPEC_IMAGE);
	use_fakes();
}

static void test_set_and_already(void)
{
	struct config cfg;
	struct eeprom_cache cache = { 0 };
	const char *result;
	char err[MSG_MAX];

	apply_setup("set_and_already", &cfg);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	CHECK_EQ(handle("eth1", &cfg, &cache), EXIT_OK);
	CHECK_STR(mac_of("eth1"), "02:9e:e6:97:4d:63");
	CHECK_EQ(g_ncalls, 1);
	CHECK_STR(g_calls[0], "eth1 address 02:9e:e6:97:4d:63");
	CHECK_EQ(apply_mac("eth1", "02:9e:e6:97:4d:63", &result, err, sizeof err), 0);
	CHECK_STR(result, "ALREADY");
	set_address("eth1", "02:9E:E6:97:4D:63");		/* sysfs case does not matter */
	CHECK_EQ(apply_mac("eth1", "02:9e:e6:97:4d:63", &result, err, sizeof err), 0);
	CHECK_STR(result, "ALREADY");
	teardown();
}

static void test_up_interface_cycled(void)
{
	struct config cfg;
	const char *result;
	char err[MSG_MAX];

	apply_setup("up_interface_cycled", &cfg);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 1);
	CHECK_EQ(apply_mac("eth1", "02:00:00:00:00:01", &result, err, sizeof err), 0);
	CHECK_STR(result, "OK");
	CHECK_EQ(g_ncalls, 3);
	CHECK_STR(g_calls[0], "eth1 down");
	CHECK_STR(g_calls[1], "eth1 address 02:00:00:00:00:01");
	CHECK_STR(g_calls[2], "eth1 up");
	teardown();
}

static void test_left_down_reported(void)
{
	struct config cfg;
	const char *result;
	char err[MSG_MAX];

	apply_setup("left_down_reported", &cfg);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 1);
	g_fail_up = 1;
	CHECK_EQ(apply_mac("eth1", "02:00:00:00:00:01", &result, err, sizeof err), -1);
	CHECK_HAS(err, "MAC set to 02:00:00:00:00:01, but interface left DOWN");

	set_address("eth1", "aa:bb:cc:dd:ee:01");
	g_fail_mac = 1;
	CHECK_EQ(apply_mac("eth1", "02:00:00:00:00:01", &result, err, sizeof err), -1);
	CHECK_HAS(err, "MAC unchanged; interface left DOWN");

	g_fail_up = 0;
	CHECK_EQ(apply_mac("eth1", "02:00:00:00:00:01", &result, err, sizeof err), -1);
	CHECK_HAS(err, "setting address failed: fake address failed; MAC unchanged");
	CHECK(!strstr(err, "DOWN"));

	g_fail_mac = 0;
	g_fail_down = 1;
	CHECK_EQ(apply_mac("eth1", "02:00:00:00:00:01", &result, err, sizeof err), -1);
	CHECK_HAS(err, "link down failed");
	teardown();
}

static void test_non_ethernet_address(void)
{
	struct config cfg;
	const char *result;
	char err[MSG_MAX];

	apply_setup("non_ethernet_address", &cfg);
	iface("ib0", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	set_address("ib0", "02:9e:e6:97:4d:63:00:00");
	CHECK_EQ(apply_mac("ib0", "02:9e:e6:97:4d:63", &result, err, sizeof err), -1);
	CHECK_HAS(err, "not an Ethernet MAC");
	CHECK_EQ(g_ncalls, 0);
	teardown();
}

static void test_deferred_without_eeprom(void)
{
	struct config cfg;
	struct eeprom_cache cache = { 0 };

	apply_setup("deferred_without_eeprom", &cfg);
	unlink(g_eeprom);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	CHECK_EQ(handle("eth1", &cfg, &cache), EXIT_DEFERRED);
	CHECK_EQ(g_ncalls, 0);
	teardown();
}

static void test_nomatch_does_not_read_eeprom(void)
{
	struct config cfg;
	struct eeprom_cache cache = { 0 };

	apply_setup("nomatch_does_not_read_eeprom", &cfg);
	iface("eth9", USB_EHCI "/1-1.4/1-1.4:1.0", "smsc95xx", NULL, 0);
	CHECK_EQ(handle("eth9", &cfg, &cache), EXIT_NOMATCH);
	CHECK_EQ(cache.loaded, 0);
	teardown();
}

static void test_all(void)
{
	struct config cfg;

	apply_setup("all", &cfg);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	iface("eth3", USB_OTG "/2-1.1/2-1.1:1.0", "smsc95xx", NULL, 0);
	iface("eth5", SPI2_DEV, "w5100", "spi:w5500", 0);
	virtual_iface("br0");
	CHECK_EQ(run_all(&cfg), EXIT_OK);
	CHECK_STR(mac_of("eth1"), "02:9e:e6:97:4d:63");
	CHECK_STR(mac_of("eth3"), "02:9e:e6:97:4d:65");
	CHECK_STR(mac_of("eth5"), "02:9e:e6:97:4d:62");
	teardown();
}

static void test_all_counts_errors(void)
{
	struct config cfg;
	uint8_t img[256];

	apply_setup("all_counts_errors", &cfg);
	patched_at(img, SPEC_IMAGE, MAC_COUNT_OFF, 5);
	eeprom(img, sizeof img);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	iface("eth3", USB_OTG "/2-1.1/2-1.1:1.0", "smsc95xx", NULL, 0);	/* slot 6 > mac_count */
	CHECK_EQ(run_all(&cfg), EXIT_ERROR);
	CHECK_STR(mac_of("eth1"), "02:9e:e6:97:4d:63");
	CHECK_STR(mac_of("eth3"), "aa:bb:cc:dd:ee:01");
	teardown();
}

static void test_check(void)
{
	struct config cfg;

	apply_setup("check", &cfg);
	iface("eth1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	CHECK_EQ(run_check(&cfg), EXIT_OK);
	unlink(g_eeprom);
	CHECK_EQ(run_check(&cfg), EXIT_ERROR);
	teardown();
}

static int run_main(int n, ...)
{
	char *argv[4] = { "napi-set-mac", NULL, NULL, NULL };
	va_list ap;
	va_start(ap, n);
	for (int i = 0; i < n; i++)
		argv[i + 1] = va_arg(ap, char *);
	va_end(ap);
	return napi_main(n + 1, argv);
}

static void test_main(void)
{
	struct config cfg;

	apply_setup("main", &cfg);
	unsetenv("ACTION");
	iface("lanusb1", USB_EHCI "/1-1.1/1-1.1:1.0", "smsc95xx", NULL, 0);
	/* udev passes the index; the name may have changed since the event. */
	CHECK_EQ(run_main(2, "--ifindex", "7"), EXIT_OK);
	CHECK_STR(mac_of("lanusb1"), "02:9e:e6:97:4d:63");
	CHECK_EQ(run_main(2, "--ifindex", "8"), EXIT_NOMATCH);
	CHECK_EQ(run_main(2, "--ifindex", "x"), EXIT_ERROR);
	CHECK_EQ(run_main(2, "--ifindex", "0"), EXIT_ERROR);
	CHECK_EQ(run_main(0), EXIT_ERROR);
	CHECK_EQ(run_main(1, "lo"), EXIT_NOMATCH);
	CHECK_EQ(run_main(1, "--bogus"), EXIT_ERROR);
	CHECK_EQ(run_main(1, "../etc"), EXIT_NOMATCH);
	CHECK_EQ(run_main(1, "lanusb1"), EXIT_OK);
	CHECK_EQ(run_main(1, "--all"), EXIT_OK);
	config("[mac]\nenabled = false\n");
	CHECK_EQ(run_main(1, "lanusb1"), EXIT_NOMATCH);
	config("[w5500_spi1]\nenabled = true\nmac_slot = 0\n");
	CHECK_EQ(run_main(1, "lanusb1"), EXIT_ERROR);
	CHECK_EQ(run_main(1, "--check"), EXIT_ERROR);
	teardown();
}

int main(void)
{
	test_crc_check_value();
	test_spec_image();
	test_eeprom_missing();
	test_eeprom_errors();
	test_slot_checks();
	test_config_full();
	test_config_missing();
	test_config_errors();
	test_config_syntax();
	test_config_disabled_not_validated();
	test_config_unknown_section();
	test_classify_usb();
	test_classify_usb_not_matched();
	test_classify_w5500();
	test_classify_native();
	test_set_and_already();
	test_up_interface_cycled();
	test_left_down_reported();
	test_non_ethernet_address();
	test_deferred_without_eeprom();
	test_nomatch_does_not_read_eeprom();
	test_all();
	test_all_counts_errors();
	test_check();
	test_main();

	printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
