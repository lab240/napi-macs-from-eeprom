SUMMARY = "Assign MAC addresses from the NAPI board EEPROM"
DESCRIPTION = "napi-set-mac reads the MAC slots of the on-board NAPI EEPROM \
(layout v3) and assigns them to the W5500 and USB Ethernet interfaces \
described in /etc/napi/mac.conf: from a udev rule on interface and EEPROM \
appearance, and from a remedial one-shot service at boot. Also names the \
interfaces lanusb1..4 / lanw5500 by controller and port."
HOMEPAGE = "https://github.com/lab240/napi-macs-from-eeprom"
SECTION = "net"

# The repository has no license file yet; replace with the real license and
# LIC_FILES_CHKSUM once one is added.
LICENSE = "CLOSED"

SRC_URI = "git://github.com/lab240/napi-macs-from-eeprom.git;protocol=https;branch=c-port"
# Development: follow the branch. For a release, pin a commit instead:
#   SRCREV = "<commit>"
SRCREV = "${AUTOREV}"
PV = "13.1+git"

S = "${WORKDIR}/git"

inherit systemd

EXTRA_OEMAKE = " \
    'CC=${CC}' \
    'CFLAGS=${CFLAGS}' \
    'LDFLAGS=${LDFLAGS}' \
    'UDEVDIR=${nonarch_base_libdir}/udev' \
    'SYSTEMDUNITDIR=${systemd_system_unitdir}' \
    'SYSCONFDIR=${sysconfdir}' \
"

do_compile() {
    oe_runmake
}

do_install() {
    oe_runmake install DESTDIR=${D}
}

SYSTEMD_SERVICE:${PN} = "napi-mac.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += " \
    ${nonarch_base_libdir}/udev/napi-set-mac \
    ${nonarch_base_libdir}/udev/rules.d/75-napi-mac.rules \
    ${nonarch_base_libdir}/udev/rules.d/99-napi-net-names.rules \
"
CONFFILES:${PN} = "${sysconfdir}/napi/mac.conf"

# Needs the at24 driver for the EEPROM (kernel-module-at24 when built as a
# module, as in Napi Linux 0.3.x) and udev/systemd from the image.
RRECOMMENDS:${PN} = "kernel-module-at24"
