/*
 *  __MODULE__	napi_mac.h
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Facility:	NAPI EEPROM MAC assignment
 *
 *  Abstract:	Shared declarations of napi-set-mac. The program assigns MAC
 *		addresses from the on-board NAPI EEPROM (layout v3, see
 *		NAPI_EEPROM_SPEC.md) to network interfaces described in
 *		/etc/napi/mac.conf. Behaviour follows the Python v12.1 helper.
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#ifndef NAPI_MAC_H
#define NAPI_MAC_H

#include <stddef.h>
#include <stdint.h>
#include <syslog.h>

#ifndef NAPI_VERSION
#define NAPI_VERSION	"13.1"
#endif

/* EEPROM v3 layout, little-endian. Only the fields this consumer uses. */
#define EEPROM_SIZE	0x7E		/* bytes covered by the v3 layout including CRC */
#define EEPROM_VERSION	3
#define PLATFORM_RK3308	1
#define VER_OFF		0x04
#define PLATFORM_OFF	0x05
#define MAC_COUNT_OFF	0x17
#define MAC_BASE_OFF	0x38
#define CRC_OFF		0x7A
#define MAC_LEN		6
#define MAC_SLOTS	8
#define NATIVE_SLOT	1

#define MAC_STR_LEN	18		/* "xx:xx:xx:xx:xx:xx" + NUL */
#define MSG_MAX		512

enum napi_exit {
	EXIT_OK		= 0,		/* MAC set or already correct */
	EXIT_ERROR	= 1,		/* EEPROM, config or interface error */
	EXIT_NOMATCH	= 3,		/* interface not managed */
	EXIT_DEFERRED	= 4		/* managed, EEPROM not available yet */
};

/* File locations; tests point them into a scratch tree. */
struct napi_paths {
	const char	*eeprom;
	const char	*config;
	const char	*sys_class_net;
};
extern struct napi_paths g_paths;

/* True when started by udev (RUN+=); logging goes to syslog then. */
extern int g_in_udev;

/* ---- configuration ---------------------------------------------------- */

#define MAX_CONTROLLERS	4
#define MAX_RULES	8
#define MAX_WARNINGS	16
#define NAME_LEN	64

enum rule_kind { RULE_NATIVE, RULE_W5500, RULE_USB };

/*
 * One enabled configuration entry.
 *
 * section	section name ("native" for the GMAC)
 * controllers	platform names of host controllers; none = any (w5500 only)
 * bus		SPI bus name (w5500)
 * port		USB port path without bus number (usb)
 * driver	expected kernel driver (usb)
 */
struct rule {
	char		section[NAME_LEN];
	enum rule_kind	kind;
	int		slot;
	int		ncontrollers;
	char		controllers[MAX_CONTROLLERS][NAME_LEN];
	char		bus[NAME_LEN];
	char		port[NAME_LEN];
	char		driver[NAME_LEN];
};

struct config {
	int		enabled;		/* [mac] enabled master switch */
	int		nrules;
	struct rule	rules[MAX_RULES];
	int		nwarnings;
	char		warnings[MAX_WARNINGS][128];
};

int config_load(const char *path, struct config *cfg, char *err, size_t errlen);

/* ---- EEPROM ----------------------------------------------------------- */

enum eeprom_status { EE_OK = 0, EE_UNAVAILABLE = 1, EE_INVALID = 2 };

uint32_t crc32_iso_hdlc(const uint8_t *p, size_t n);
int eeprom_validate(const uint8_t *b, size_t len, char *err, size_t errlen);
int eeprom_read(const char *path, uint8_t b[EEPROM_SIZE], char *err, size_t errlen);
int eeprom_get_mac(const uint8_t *b, int slot, char out[MAC_STR_LEN], char *err, size_t errlen);

/* ---- interface classification ----------------------------------------- */

int usb_match(const char *dev, const struct rule *r);
const struct rule *classify(const char *iface, const struct config *cfg);

/* ---- interface operations --------------------------------------------- */

/*
 * Operations that change kernel state; tests substitute fakes.
 *
 * set_up	bring interface up (up=1) or down (up=0)
 * set_mac	assign "xx:xx:xx:xx:xx:xx"
 * indextoname	if_indextoname(3)
 * All return 0 on success, -1 with a message in err.
 */
struct netops {
	int	(*set_up)(const char *iface, int up, char *err, size_t errlen);
	int	(*set_mac)(const char *iface, const char *mac, char *err, size_t errlen);
	char	*(*indextoname)(unsigned idx, char *name);
};
extern const struct netops *g_netops;
extern const struct netops real_netops;

int sysfs_read_line(const char *path, char *buf, size_t len);
int current_mac(const char *iface, char out[MAC_STR_LEN], char *err, size_t errlen);
int apply_mac(const char *iface, const char *target, const char **result, char *err, size_t errlen);

/* ---- driver ----------------------------------------------------------- */

struct eeprom_cache {
	int		loaded;
	int		status;
	uint8_t		b[EEPROM_SIZE];
	char		err[MSG_MAX];
};

int handle(const char *iface, const struct config *cfg, struct eeprom_cache *cache);
int run_all(const struct config *cfg);
int run_check(const struct config *cfg);
int napi_main(int argc, char **argv);

void logmsg(int priority, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* NAPI_MAC_H */
