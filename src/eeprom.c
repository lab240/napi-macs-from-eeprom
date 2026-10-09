/*
 *  __MODULE__	eeprom.c
 *  __IDENT__	V13-000
 *  __REV__	13.0
 *
 *  Abstract:	Read and validate the NAPI EEPROM (layout v3) and extract MAC
 *		addresses. Checks follow NAPI_EEPROM_SPEC.md section 8: magic,
 *		version, CRC before any field is trusted, then platform_id and
 *		mac_count.
 *
 *  Modification history:
 *	13.0	09-OCT-2026	C port of napi-set-mac v12.1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "napi_mac.h"

static const uint8_t EEPROM_MAGIC[4] = { 'N', 'A', 'P', 'I' };

/*
 * CRC-32/ISO-HDLC, identical to zlib.crc32(); reference algorithm from
 * NAPI_EEPROM_SPEC.md section 7.
 */
uint32_t crc32_iso_hdlc(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;

	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	return crc ^ 0xFFFFFFFFu;
}

static const char *base_dir(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static int all_bytes(const uint8_t *b, size_t n, uint8_t v)
{
	for (size_t i = 0; i < n; i++)
		if (b[i] != v)
			return 0;
	return 1;
}

/*
 * Validate an EEPROM image.
 *
 * a_b		image bytes
 * a_len	number of bytes read
 * Returns	EE_OK or EE_INVALID with a message in err
 */
int eeprom_validate(const uint8_t *b, size_t len, char *err, size_t errlen)
{
	if (len < EEPROM_SIZE) {
		snprintf(err, errlen, "EEPROM short read (%zu of %d bytes)", len, EEPROM_SIZE);
		return EE_INVALID;
	}
	if (all_bytes(b, EEPROM_SIZE, 0x00) || all_bytes(b, EEPROM_SIZE, 0xFF)) {
		snprintf(err, errlen, "EEPROM uninitialized (all bytes %02x)", b[0]);
		return EE_INVALID;
	}
	if (memcmp(b, EEPROM_MAGIC, sizeof EEPROM_MAGIC) != 0) {
		snprintf(err, errlen, "EEPROM magic invalid");
		return EE_INVALID;
	}
	if (b[VER_OFF] != EEPROM_VERSION) {
		snprintf(err, errlen, "EEPROM layout v%u unsupported (need v%d)", b[VER_OFF], EEPROM_VERSION);
		return EE_INVALID;
	}
	/* CRC first: field values are meaningless until integrity is confirmed. */
	uint32_t stored = (uint32_t)b[CRC_OFF] | (uint32_t)b[CRC_OFF + 1] << 8 |
			  (uint32_t)b[CRC_OFF + 2] << 16 | (uint32_t)b[CRC_OFF + 3] << 24;
	uint32_t calc = crc32_iso_hdlc(b, CRC_OFF);
	if (stored != calc) {
		snprintf(err, errlen, "EEPROM CRC invalid (stored %08x, computed %08x)", stored, calc);
		return EE_INVALID;
	}
	/* This consumer knows only the RK3308 interface set (GMAC driver name, SPI/USB paths). */
	if (b[PLATFORM_OFF] != PLATFORM_RK3308) {
		snprintf(err, errlen, "EEPROM platform_id %u unsupported (need %d)", b[PLATFORM_OFF], PLATFORM_RK3308);
		return EE_INVALID;
	}
	if (b[MAC_COUNT_OFF] > MAC_SLOTS) {
		snprintf(err, errlen, "EEPROM mac_count %u exceeds %d", b[MAC_COUNT_OFF], MAC_SLOTS);
		return EE_INVALID;
	}
	return EE_OK;
}

/*
 * Read and validate the NAPI EEPROM header.
 *
 * a_path	EEPROM sysfs file
 * a_b		receives exactly EEPROM_SIZE bytes
 * Returns	EE_OK; EE_UNAVAILABLE if the file does not exist (yet);
 *		EE_INVALID on read errors or failed checks. err holds the reason.
 */
int eeprom_read(const char *path, uint8_t b[EEPROM_SIZE], char *err, size_t errlen)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT) {
			/*
			 * Tell "not in the device tree" (never appears by
			 * itself) from "driver not bound yet" (at24 module).
			 */
			char dir[PATH_MAX];
			struct stat st;
			snprintf(dir, sizeof dir, "%s", path);
			char *slash = strrchr(dir, '/');
			if (slash)
				*slash = '\0';
			if (slash && stat(dir, &st) < 0)
				snprintf(err, errlen, "EEPROM %s not available: no I2C device %s "
					 "(EEPROM not in the device tree, overlay i2c1-at24 not enabled?)",
					 path, base_dir(dir));
			else
				snprintf(err, errlen, "EEPROM %s not available: at24 driver not bound yet", path);
			return EE_UNAVAILABLE;
		}
		snprintf(err, errlen, "EEPROM %s: %s", path, strerror(errno));
		return EE_INVALID;
	}
	/* Never read the whole chip, I2C is slow. */
	size_t got = 0;
	while (got < EEPROM_SIZE) {
		ssize_t r = read(fd, b + got, EEPROM_SIZE - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			snprintf(err, errlen, "EEPROM %s: %s", path, strerror(errno));
			close(fd);
			return EE_INVALID;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	return eeprom_validate(b, got, err, errlen);
}

static const uint8_t *raw_mac(const uint8_t *b, int slot)
{
	return b + MAC_BASE_OFF + (slot - 1) * MAC_LEN;
}

/*
 * Extract one MAC address from a validated EEPROM image.
 *
 * a_slot	1-based slot number
 * a_out	"xx:xx:xx:xx:xx:xx" lowercase
 * Returns	0, or -1 if the slot is absent or holds an unusable address
 */
int eeprom_get_mac(const uint8_t *b, int slot, char out[MAC_STR_LEN], char *err, size_t errlen)
{
	if (slot < 1 || slot > MAC_SLOTS) {
		snprintf(err, errlen, "MAC slot %d out of range 1..%d", slot, MAC_SLOTS);
		return -1;
	}
	int count = b[MAC_COUNT_OFF];
	if (count < slot) {
		snprintf(err, errlen, "MAC slot %d not programmed (EEPROM holds %d)", slot, count);
		return -1;
	}
	const uint8_t *m = raw_mac(b, slot);
	if (all_bytes(m, MAC_LEN, 0x00) || all_bytes(m, MAC_LEN, 0xFF)) {
		snprintf(err, errlen, "MAC slot %d is blank", slot);
		return -1;
	}
	if (m[0] & 0x01) {
		snprintf(err, errlen, "MAC slot %d is a multicast address", slot);
		return -1;
	}
	for (int s = 1; s <= count; s++) {
		if (s != slot && memcmp(raw_mac(b, s), m, MAC_LEN) == 0) {
			snprintf(err, errlen, "MAC slot %d duplicates slot %d", slot, s);
			return -1;
		}
	}
	snprintf(out, MAC_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return 0;
}
