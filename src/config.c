/*
 *  __MODULE__	config.c
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Abstract:	Read /etc/napi/mac.conf and build the list of enabled rules.
 *		The INI dialect is the subset of Python configparser that the
 *		v12 helper accepted: "[section]", "key = value" or "key: value",
 *		full-line comments starting with '#' or ';', keys
 *		case-insensitive, duplicate sections/keys rejected. Inline
 *		comments and continuation lines are not supported.
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "napi_mac.h"

#define INI_SECTIONS	32
#define INI_KEYS	32
#define INI_VALUE	256

struct ini_kv {
	char	key[NAME_LEN];
	char	value[INI_VALUE];
};

struct ini_section {
	char		name[NAME_LEN];
	int		nkeys;
	struct ini_kv	kv[INI_KEYS];
};

struct ini {
	int			nsections;
	struct ini_section	s[INI_SECTIONS];
};

/* Sections describing W5500 SPI controllers and their defaults. */
static const struct {
	const char	*section;
	const char	*bus;
	int		slot;
} W5500_SECTIONS[] = {
	{ "w5500_spi1", "spi1", 2 },
	{ "w5500_spi2", "spi2", 3 },
};

/* Sections describing built-in USB Ethernet adapters. */
static const char *const USB_SECTIONS[] = { "usb_eth_1", "usb_eth_2", "usb_eth_3", "usb_eth_4" };

#define NELEM(a)	(sizeof(a) / sizeof((a)[0]))

static char *strip(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static int copy(char *dst, size_t len, const char *src)
{
	if (strlen(src) >= len)
		return -1;
	strcpy(dst, src);
	return 0;
}

static struct ini_section *ini_find(struct ini *ini, const char *name)
{
	for (int i = 0; i < ini->nsections; i++)
		if (strcmp(ini->s[i].name, name) == 0)
			return &ini->s[i];
	return NULL;
}

/*
 * Parse an INI file.
 *
 * Returns	0, or -1 with "line N: reason" in err
 */
static int ini_parse(FILE *f, struct ini *ini, char *err, size_t errlen)
{
	char line[1024];
	int lineno = 0;
	struct ini_section *cur = NULL;

	ini->nsections = 0;
	while (fgets(line, sizeof line, f)) {
		lineno++;
		size_t len = strlen(line);
		if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(f)) {
			snprintf(err, errlen, "line %d: too long", lineno);
			return -1;
		}
		int indented = line[0] == ' ' || line[0] == '\t';
		char *t = strip(line);
		if (*t == '\0' || *t == '#' || *t == ';')
			continue;
		if (indented) {
			snprintf(err, errlen, "line %d: indented continuation lines are not supported", lineno);
			return -1;
		}

		if (*t == '[') {
			char *e = strrchr(t, ']');
			if (!e || e == t + 1) {
				snprintf(err, errlen, "line %d: bad section header '%s'", lineno, t);
				return -1;
			}
			*e = '\0';
			const char *name = t + 1;
			if (ini_find(ini, name)) {
				snprintf(err, errlen, "line %d: section [%s] already exists", lineno, name);
				return -1;
			}
			if (ini->nsections == INI_SECTIONS) {
				snprintf(err, errlen, "line %d: too many sections", lineno);
				return -1;
			}
			cur = &ini->s[ini->nsections];
			if (copy(cur->name, sizeof cur->name, name) < 0) {
				snprintf(err, errlen, "line %d: section name too long", lineno);
				return -1;
			}
			cur->nkeys = 0;
			ini->nsections++;
			continue;
		}

		if (!cur) {
			snprintf(err, errlen, "line %d: key outside of any [section]", lineno);
			return -1;
		}
		char *d = strpbrk(t, "=:");
		if (!d) {
			snprintf(err, errlen, "line %d: expected 'key = value', got '%s'", lineno, t);
			return -1;
		}
		*d = '\0';
		char *key = strip(t);
		char *value = strip(d + 1);
		if (*key == '\0') {
			snprintf(err, errlen, "line %d: empty key", lineno);
			return -1;
		}
		for (char *p = key; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		for (int i = 0; i < cur->nkeys; i++) {
			if (strcmp(cur->kv[i].key, key) == 0) {
				snprintf(err, errlen, "line %d: duplicate key '%s' in [%s]", lineno, key, cur->name);
				return -1;
			}
		}
		if (cur->nkeys == INI_KEYS) {
			snprintf(err, errlen, "line %d: too many keys in [%s]", lineno, cur->name);
			return -1;
		}
		struct ini_kv *kv = &cur->kv[cur->nkeys];
		if (copy(kv->key, sizeof kv->key, key) < 0 || copy(kv->value, sizeof kv->value, value) < 0) {
			snprintf(err, errlen, "line %d: key or value too long", lineno);
			return -1;
		}
		cur->nkeys++;
	}
	if (ferror(f)) {
		snprintf(err, errlen, "read error: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Raw option value (possibly empty), or NULL if the option is absent. */
static const char *ini_get(struct ini *ini, const char *sec, const char *key)
{
	struct ini_section *s = ini_find(ini, sec);
	if (!s)
		return NULL;
	for (int i = 0; i < s->nkeys; i++)
		if (strcmp(s->kv[i].key, key) == 0)
			return s->kv[i].value;
	return NULL;
}

/* Non-empty value; missing or empty is an error only when required. */
static int get_str(struct ini *ini, const char *sec, const char *key, int required,
		   const char **out, char *err, size_t errlen)
{
	const char *v = ini_get(ini, sec, key);
	if (v && *v) {
		*out = v;
		return 0;
	}
	*out = NULL;
	if (required) {
		snprintf(err, errlen, "config: [%s] missing required key '%s'", sec, key);
		return -1;
	}
	return 0;
}

static int get_bool(struct ini *ini, const char *sec, const char *key, int fallback,
		    int *out, char *err, size_t errlen)
{
	static const char *const yes[] = { "1", "yes", "true", "on" };
	static const char *const no[] = { "0", "no", "false", "off" };
	const char *v = ini_get(ini, sec, key);

	if (!v) {
		*out = fallback;
		return 0;
	}
	for (size_t i = 0; i < NELEM(yes); i++) {
		if (strcasecmp(v, yes[i]) == 0) {
			*out = 1;
			return 0;
		}
		if (strcasecmp(v, no[i]) == 0) {
			*out = 0;
			return 0;
		}
	}
	snprintf(err, errlen, "config: [%s] %s = '%s' is not a boolean", sec, key, v);
	return -1;
}

static int get_slot(struct ini *ini, const char *sec, int required, int fallback,
		    int *out, char *err, size_t errlen)
{
	const char *raw;
	if (get_str(ini, sec, "mac_slot", required, &raw, err, errlen) < 0)
		return -1;
	if (!raw) {
		*out = fallback;
		return 0;
	}
	char *end;
	errno = 0;
	long v = strtol(raw, &end, 10);
	if (errno || end == raw || *end != '\0') {
		snprintf(err, errlen, "config: [%s] mac_slot = '%s' is not a number", sec, raw);
		return -1;
	}
	if (v < 1 || v > MAC_SLOTS) {
		snprintf(err, errlen, "config: [%s] mac_slot %ld out of range 1..%d", sec, v, MAC_SLOTS);
		return -1;
	}
	*out = (int)v;
	return 0;
}

static int set_field(char *dst, size_t len, const char *sec, const char *key, const char *v,
		     char *err, size_t errlen)
{
	if (copy(dst, len, v) < 0) {
		snprintf(err, errlen, "config: [%s] %s value too long", sec, key);
		return -1;
	}
	return 0;
}

/* Whitespace- or comma-separated list of controllers. */
static int get_controllers(struct ini *ini, const char *sec, int required, struct rule *r,
			   char *err, size_t errlen)
{
	const char *raw;
	if (get_str(ini, sec, "controller", required, &raw, err, errlen) < 0)
		return -1;
	r->ncontrollers = 0;
	if (!raw)
		return 0;
	char buf[INI_VALUE];
	strcpy(buf, raw);
	for (char *save = NULL, *tok = strtok_r(buf, " \t,", &save); tok; tok = strtok_r(NULL, " \t,", &save)) {
		if (r->ncontrollers == MAX_CONTROLLERS) {
			snprintf(err, errlen, "config: [%s] more than %d controllers", sec, MAX_CONTROLLERS);
			return -1;
		}
		if (set_field(r->controllers[r->ncontrollers], NAME_LEN, sec, "controller", tok, err, errlen) < 0)
			return -1;
		r->ncontrollers++;
	}
	if (required && r->ncontrollers == 0) {
		snprintf(err, errlen, "config: [%s] missing required key 'controller'", sec);
		return -1;
	}
	return 0;
}

static int is_known_section(const char *name)
{
	if (strcmp(name, "mac") == 0)
		return 1;
	for (size_t i = 0; i < NELEM(W5500_SECTIONS); i++)
		if (strcmp(name, W5500_SECTIONS[i].section) == 0)
			return 1;
	for (size_t i = 0; i < NELEM(USB_SECTIONS); i++)
		if (strcmp(name, USB_SECTIONS[i]) == 0)
			return 1;
	return 0;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Two USB rules on the same port of a shared controller would race for one adapter. */
static int check_usb_overlap(const struct config *cfg, char *err, size_t errlen)
{
	for (int i = 0; i < cfg->nrules; i++) {
		const struct rule *a = &cfg->rules[i];
		if (a->kind != RULE_USB)
			continue;
		for (int j = i + 1; j < cfg->nrules; j++) {
			const struct rule *b = &cfg->rules[j];
			if (b->kind != RULE_USB || strcmp(a->port, b->port) != 0)
				continue;
			const char *common[MAX_CONTROLLERS];
			int n = 0;
			for (int x = 0; x < a->ncontrollers; x++) {
				int dup = 0;
				for (int k = 0; k < n; k++)
					dup |= strcmp(common[k], a->controllers[x]) == 0;
				for (int y = 0; y < b->ncontrollers && !dup; y++) {
					if (strcmp(a->controllers[x], b->controllers[y]) == 0) {
						common[n++] = a->controllers[x];
						break;
					}
				}
			}
			if (n == 0)
				continue;
			qsort(common, (size_t)n, sizeof common[0], cmp_str);
			char list[MAX_CONTROLLERS * NAME_LEN] = "";
			for (int k = 0; k < n; k++) {
				if (k)
					strcat(list, " ");
				strcat(list, common[k]);
			}
			snprintf(err, errlen, "config: [%s] and [%s] both match port %s on %s",
				 a->section, b->section, a->port, list);
			return -1;
		}
	}
	return 0;
}

/*
 * Read the configuration and build the list of enabled rules.
 *
 * a_path	config file
 * a_cfg	receives the master switch, rules and non-fatal warnings
 * Returns	0, or -1 with the reason in err: syntax errors, bad values,
 *		missing keys, duplicate effective slots, overlapping USB rules.
 *		A missing file is logged and yields enabled = 0, no rules.
 */
int config_load(const char *path, struct config *cfg, char *err, size_t errlen)
{
	static struct ini ini;		/* ~0.5 MB; keep it off the stack */
	char perr[MSG_MAX];

	memset(cfg, 0, sizeof *cfg);
	FILE *f = fopen(path, "re");
	if (!f) {
		if (errno == ENOENT)
			logmsg(LOG_WARNING, "config: %s not found, no interfaces managed", path);
		else
			logmsg(LOG_WARNING, "config: %s: %s, no interfaces managed", path, strerror(errno));
		return 0;
	}
	int rc = ini_parse(f, &ini, perr, sizeof perr);
	fclose(f);
	if (rc < 0) {
		snprintf(err, errlen, "config: %s: %s", path, perr);
		return -1;
	}

	for (int i = 0; i < ini.nsections; i++) {
		if (!is_known_section(ini.s[i].name) && cfg->nwarnings < MAX_WARNINGS)
			snprintf(cfg->warnings[cfg->nwarnings++], sizeof cfg->warnings[0],
				 "config: unknown section [%.64s] ignored", ini.s[i].name);
	}

	cfg->enabled = 1;
	int native = 0;
	if (ini_find(&ini, "mac")) {
		if (get_bool(&ini, "mac", "enabled", 1, &cfg->enabled, err, errlen) < 0 ||
		    get_bool(&ini, "mac", "set_native_eth_mac", 0, &native, err, errlen) < 0)
			return -1;
	}
	if (native) {
		struct rule *r = &cfg->rules[cfg->nrules++];
		strcpy(r->section, "native");
		r->kind = RULE_NATIVE;
		r->slot = NATIVE_SLOT;
	}

	for (size_t i = 0; i < NELEM(W5500_SECTIONS); i++) {
		const char *sec = W5500_SECTIONS[i].section;
		int on;
		if (!ini_find(&ini, sec))
			continue;
		if (get_bool(&ini, sec, "enabled", 0, &on, err, errlen) < 0)
			return -1;
		if (!on)
			continue;
		struct rule *r = &cfg->rules[cfg->nrules++];
		const char *bus;
		strcpy(r->section, sec);
		r->kind = RULE_W5500;
		if (get_slot(&ini, sec, 0, W5500_SECTIONS[i].slot, &r->slot, err, errlen) < 0 ||
		    get_controllers(&ini, sec, 0, r, err, errlen) < 0 ||
		    get_str(&ini, sec, "bus", 0, &bus, err, errlen) < 0 ||
		    set_field(r->bus, sizeof r->bus, sec, "bus", bus ? bus : W5500_SECTIONS[i].bus, err, errlen) < 0)
			return -1;
	}

	for (size_t i = 0; i < NELEM(USB_SECTIONS); i++) {
		const char *sec = USB_SECTIONS[i];
		int on;
		if (!ini_find(&ini, sec))
			continue;
		if (get_bool(&ini, sec, "enabled", 0, &on, err, errlen) < 0)
			return -1;
		if (!on)
			continue;
		struct rule *r = &cfg->rules[cfg->nrules++];
		const char *port, *driver;
		strcpy(r->section, sec);
		r->kind = RULE_USB;
		if (get_slot(&ini, sec, 1, 0, &r->slot, err, errlen) < 0 ||
		    get_controllers(&ini, sec, 1, r, err, errlen) < 0 ||
		    get_str(&ini, sec, "port", 1, &port, err, errlen) < 0 ||
		    set_field(r->port, sizeof r->port, sec, "port", port, err, errlen) < 0 ||
		    get_str(&ini, sec, "driver", 0, &driver, err, errlen) < 0 ||
		    set_field(r->driver, sizeof r->driver, sec, "driver", driver ? driver : "smsc95xx", err, errlen) < 0)
			return -1;
	}

	/*
	 * Every rule must own a distinct effective slot (defaults and the
	 * native slot included), otherwise two interfaces would get the same
	 * address.
	 */
	for (int i = 0; i < cfg->nrules; i++) {
		for (int j = 0; j < i; j++) {
			if (cfg->rules[i].slot == cfg->rules[j].slot) {
				snprintf(err, errlen, "config: MAC slot %d used by both [%s] and [%s]",
					 cfg->rules[i].slot, cfg->rules[j].section, cfg->rules[i].section);
				return -1;
			}
		}
	}
	return check_usb_overlap(cfg, err, errlen);
}
