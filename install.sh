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
#	12.3	09-OCT-2026	99-napi-net-names.rules (names by controller and
#				port); a foreign rule of that name is kept as
#				/etc/napi/99-napi-net-names.rules.orig;
#				update-initramfs -u where available, because
#				Debian/Armbian copy /etc/udev/rules.d into it.
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

# Interface names. Keep a rule of the same name that is not ours (Armbian
# image) so that uninstall.sh can put it back.
l_names=/etc/udev/rules.d/99-napi-net-names.rules
if [ -e "$l_names" ] && ! grep -q "napi-macs-from-eeprom" "$l_names" && [ ! -e /etc/napi/99-napi-net-names.rules.orig ]; then
	install -D -m644 "$l_names" /etc/napi/99-napi-net-names.rules.orig
	echo "Previous $l_names saved as /etc/napi/99-napi-net-names.rules.orig"
fi
install -D -m644 "$B/etc/udev/rules.d/99-napi-net-names.rules"		"$l_names"

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

# Debian/Armbian copy /etc/udev/rules.d into the initramfs, where interfaces
# are already renamed; refresh it so the new names apply from the next boot.
if command -v update-initramfs >/dev/null 2>&1; then
	echo "Updating initramfs..."
	update-initramfs -u >/dev/null
fi

echo "NAPI MAC v12.3 installed."
echo "Check config/EEPROM:  /usr/lib/udev/napi-set-mac --check"
echo "Test without reboot:  systemctl restart napi-mac.service; ip -br link"
echo "Interface names (lanusb1..4, lanw5500) apply from the next boot."
echo "Test udev path:       udevadm trigger --subsystem-match=net --action=add; journalctl -t napi-set-mac"
echo "Uninstall:            ./uninstall.sh [--purge]"
