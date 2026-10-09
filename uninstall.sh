#!/bin/sh
#
#  __MODULE__	uninstall.sh
#  __IDENT__	V13-001
#  __REV__	13.1
#
#  Abstract:	Remove NAPI EEPROM MAC assignment v13 (udev rule, binary,
#		remedial service) and leftovers of earlier versions.
#		/etc/napi/mac.conf is kept unless "--purge" is given.
#		MAC addresses already applied to running interfaces are not
#		reverted; they return to driver defaults on the next boot.
#
#  Usage:	./uninstall.sh [--purge]
#
#  Modification history:
#	13.1	09-OCT-2026	Remove 99-napi-net-names.rules, restore the one saved
#				by install.sh; update-initramfs -u where available.
#	13.0	09-OCT-2026	Version bump; v12 wrapper still removed as a leftover.
#	12.0	07-OCT-2026	--purge no longer touches mac.conf.bak-* files that
#				install.sh never creates.
#	11.0	24-SEP-2026	Initial version.
#
set -eu

l_purge=0
[ "${1:-}" = "--purge" ] && l_purge=1

# Service: stop is harmless (oneshot, RemainAfterExit), disable drops the symlink.
if systemctl list-unit-files napi-mac.service >/dev/null 2>&1; then
	systemctl disable --now napi-mac.service 2>/dev/null || true
fi

# v11..v13 files
rm -f /etc/udev/rules.d/75-napi-mac.rules
rm -f /usr/lib/udev/napi-set-mac
rm -f /usr/lib/napi/napi-set-macs
rm -f /usr/lib/systemd/system/napi-mac.service
rmdir /usr/lib/napi 2>/dev/null || true

# Interface names: ours goes, a previously installed one comes back.
l_names=/etc/udev/rules.d/99-napi-net-names.rules
if [ -e "$l_names" ] && grep -q "napi-macs-from-eeprom" "$l_names"; then
	rm -f "$l_names"
fi
if [ -e /etc/napi/99-napi-net-names.rules.orig ]; then
	mv /etc/napi/99-napi-net-names.rules.orig "$l_names"
	echo "Previous $l_names restored."
fi

# Leftovers of v6 hotplug experiments and earlier installs
rm -f /etc/udev/rules.d/05-napi-mac.rules
rm -f /usr/lib/systemd/system/napi-set-mac@.service

udevadm control --reload-rules
systemctl daemon-reload

# The initramfs holds copies of /etc/udev/rules.d (Debian/Armbian).
if command -v update-initramfs >/dev/null 2>&1; then
	echo "Updating initramfs..."
	update-initramfs -u >/dev/null
fi

if [ "$l_purge" -eq 1 ]; then
	rm -f /etc/napi/mac.conf /etc/napi/mac.conf.dist
	rmdir /etc/napi 2>/dev/null || true
	echo "NAPI MAC removed, configuration purged."
else
	echo "NAPI MAC removed. Configuration kept in /etc/napi (use --purge to delete)."
fi
echo "Interfaces keep their current MACs until reboot."
