#!/bin/sh
#
#  __MODULE__	install.sh
#  __IDENT__	V12-000
#  __REV__	12.0
#
#  Abstract:	Install NAPI EEPROM MAC assignment v12 (udev rule + remedial service).
#		An existing /etc/napi/mac.conf is preserved; the new default is put
#		next to it as mac.conf.dist.
#
#  Modification history:
#	12.0	07-OCT-2026	Check for python3; config check hint.
#	11.0	24-SEP-2026	Uninstall hint.
#	10.0	24-SEP-2026	udev rule; existing mac.conf preserved.
#
set -eu
B="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ ! -x /usr/bin/python3 ]; then
	echo "napi-mac: /usr/bin/python3 is required" >&2
	exit 1
fi

install -D -m755 "$B/usr/lib/udev/napi-set-mac"			/usr/lib/udev/napi-set-mac
install -D -m755 "$B/usr/lib/napi/napi-set-macs"			/usr/lib/napi/napi-set-macs
install -D -m644 "$B/usr/lib/systemd/system/napi-mac.service"	/usr/lib/systemd/system/napi-mac.service
install -D -m644 "$B/etc/udev/rules.d/75-napi-mac.rules"		/etc/udev/rules.d/75-napi-mac.rules

# Remove leftovers of v6 hotplug experiments.
rm -f /etc/udev/rules.d/05-napi-mac.rules
rm -f /usr/lib/systemd/system/napi-set-mac@.service

if [ -e /etc/napi/mac.conf ]; then
	install -D -m644 "$B/etc/napi/mac.conf" /etc/napi/mac.conf.dist
	echo "Existing /etc/napi/mac.conf kept; new default saved as /etc/napi/mac.conf.dist"
else
	install -D -m644 "$B/etc/napi/mac.conf" /etc/napi/mac.conf
fi

udevadm control --reload-rules
systemctl daemon-reload
systemctl enable napi-mac.service

echo "NAPI MAC v12 installed."
echo "Check config/EEPROM:  /usr/lib/udev/napi-set-mac --check"
echo "Test without reboot:  systemctl restart napi-mac.service; ip -br link"
echo "Test udev path:       udevadm trigger --subsystem-match=net --action=add; journalctl -t napi-set-mac"
echo "Uninstall:            ./uninstall.sh [--purge]"
