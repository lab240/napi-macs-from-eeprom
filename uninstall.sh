#!/bin/sh
#
#  __MODULE__	uninstall.sh
#  __IDENT__	V11-000
#  __REV__	11.0
#
#  Abstract:	Remove NAPI EEPROM MAC assignment v11 (udev rule, scripts,
#		remedial service) and leftovers of earlier versions.
#		/etc/napi/mac.conf is kept unless "--purge" is given.
#		MAC addresses already applied to running interfaces are not
#		reverted; they return to driver defaults on the next boot.
#
#  Usage:	./uninstall.sh [--purge]
#
#  Modification history:
#	11.0	24-SEP-2026	Initial version.
#
set -eu

l_purge=0
[ "${1:-}" = "--purge" ] && l_purge=1

# Service: stop is harmless (oneshot, RemainAfterExit), disable drops the symlink.
if systemctl list-unit-files napi-mac.service >/dev/null 2>&1; then
	systemctl disable --now napi-mac.service 2>/dev/null || true
fi

# v11 files
rm -f /etc/udev/rules.d/75-napi-mac.rules
rm -f /usr/lib/udev/napi-set-mac
rm -f /usr/lib/napi/napi-set-macs
rm -f /usr/lib/systemd/system/napi-mac.service
rmdir /usr/lib/napi 2>/dev/null || true

# Leftovers of v6 hotplug experiments and earlier installs
rm -f /etc/udev/rules.d/05-napi-mac.rules
rm -f /usr/lib/systemd/system/napi-set-mac@.service

udevadm control --reload-rules
systemctl daemon-reload

if [ "$l_purge" -eq 1 ]; then
	rm -f /etc/napi/mac.conf /etc/napi/mac.conf.dist /etc/napi/mac.conf.bak-*
	rmdir /etc/napi 2>/dev/null || true
	echo "NAPI MAC removed, configuration purged."
else
	echo "NAPI MAC removed. Configuration kept in /etc/napi (use --purge to delete)."
fi
echo "Interfaces keep their current MACs until reboot."
