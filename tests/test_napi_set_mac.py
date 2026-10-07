#!/usr/bin/python3
#
#  Unit tests for usr/lib/udev/napi-set-mac.
#
#  Run:	python3 -m unittest discover -s tests -v
#
#  EEPROM parsing is checked against the reference image from section 11 of
#  NAPI_EEPROM_SPEC.md; interface classification runs on a fake sysfs tree.
#

import binascii
import importlib.machinery
import importlib.util
import os
import pathlib
import struct
import sys
import tempfile
import textwrap
import unittest

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parent.parent
_loader = importlib.machinery.SourceFileLoader("napi_set_mac", str(ROOT / "usr/lib/udev/napi-set-mac"))
_spec = importlib.util.spec_from_loader("napi_set_mac", _loader)
m = importlib.util.module_from_spec(_spec)
_loader.exec_module(m)
m.IN_UDEV = False

# NAPI_EEPROM_SPEC.md section 11, bytes 0x00..0x7F; 0x80..0xFF are zero.
SPEC_IMAGE = bytes.fromhex("""
4e 41 50 49 03 01 05 00 00 00 01 00 02 45 00 00
05 00 00 00 1a 09 11 08 46 43 43 52 33 33 30 38
2d 52 4f 55 54 45 52 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 02 9e e6 97 4d 60 02 9e
e6 97 4d 61 02 9e e6 97 4d 62 02 9e e6 97 4d 63
02 9e e6 97 4d 64 02 9e e6 97 4d 65 02 9e e6 97
4d 66 02 9e e6 97 4d 67 01 05 09 0b 13 1d 04 00
00 00 00 00 00 00 00 00 00 00 41 0e 3a e5 00 00
""") + bytes(128)

USB_EHCI = "devices/platform/ff440000.usb/usb1/1-1"
USB_OTG = "devices/platform/ff400000.usb/usb2/2-1"


def patched(img):
    """Copy of img with a recomputed CRC."""
    b = bytearray(img)
    struct.pack_into("<I", b, m.CRC_OFF, binascii.crc32(bytes(b[:m.CRC_OFF])) & 0xFFFFFFFF)
    return bytes(b)


class TmpDir(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._td.name)
        self._saved = (m.EEPROM_PATH, m.CONFIG_PATH, m.SYS_CLASS_NET)
        m.EEPROM_PATH = str(self.tmp / "eeprom")
        m.CONFIG_PATH = str(self.tmp / "mac.conf")
        m.SYS_CLASS_NET = str(self.tmp / "sys/class/net")
        os.makedirs(m.SYS_CLASS_NET)

    def tearDown(self):
        m.EEPROM_PATH, m.CONFIG_PATH, m.SYS_CLASS_NET = self._saved
        self._td.cleanup()

    def eeprom(self, data):
        pathlib.Path(m.EEPROM_PATH).write_bytes(data)

    def config(self, text):
        pathlib.Path(m.CONFIG_PATH).write_text(textwrap.dedent(text))

    def iface(self, name, devpath, driver, modalias="", mac="aa:bb:cc:dd:ee:01", up=False):
        """Create /sys/class/net/<name> pointing to sys/<devpath>."""
        sysroot = self.tmp / "sys"
        dev = sysroot / devpath
        dev.mkdir(parents=True, exist_ok=True)
        if driver:
            drv = sysroot / "bus/drivers" / driver
            drv.mkdir(parents=True, exist_ok=True)
            (dev / "driver").symlink_to(drv)
        if modalias:
            (dev / "modalias").write_text(modalias + "\n")
        net = pathlib.Path(m.SYS_CLASS_NET, name)
        net.mkdir()
        (net / "device").symlink_to(dev)
        (net / "address").write_text(mac + "\n")
        (net / "flags").write_text("0x1003\n" if up else "0x1002\n")

    def virtual(self, name):
        net = pathlib.Path(m.SYS_CLASS_NET, name)
        net.mkdir()
        (net / "address").write_text("00:00:00:00:00:00\n")
        (net / "flags").write_text("0x1003\n")


class FakeIp:
    """Stand-in for ip(8) acting on the fake sysfs; fail = set of verbs to fail."""

    def __init__(self, fail=()):
        self.calls = []
        self.fail = set(fail)

    def __call__(self, *args):
        self.calls.append(args)
        verb = args[-1] if args[-1] in ("up", "down") else "address"
        if verb in self.fail:
            raise RuntimeError(f"fake ip {verb} failed")
        if verb == "address":
            pathlib.Path(m.SYS_CLASS_NET, args[3], "address").write_text(args[5] + "\n")


class TestEeprom(TmpDir):
    def test_spec_image(self):
        self.eeprom(SPEC_IMAGE)
        b = m.read_eeprom()
        self.assertEqual(struct.unpack_from("<I", b, m.CRC_OFF)[0], 0xE53A0E41)
        self.assertEqual([m.get_mac(b, s) for s in (1, 8)], ["02:9e:e6:97:4d:60", "02:9e:e6:97:4d:67"])

    def test_reads_only_v3_area(self):
        self.eeprom(SPEC_IMAGE[:m.EEPROM_SIZE])
        m.read_eeprom()

    def test_missing(self):
        with self.assertRaises(m.EepromUnavailable):
            m.read_eeprom()

    def test_errors(self):
        cases = {
            "short read": SPEC_IMAGE[:0x7D],
            "uninitialized": b"\xff" * 256,
            "magic": b"XAPI" + SPEC_IMAGE[4:],
            "layout v2": patched_at(SPEC_IMAGE, m.VER_OFF, 2),
            "CRC invalid": SPEC_IMAGE[:0x40] + b"\x00" + SPEC_IMAGE[0x41:],
            "platform_id 2": patched_at(SPEC_IMAGE, m.PLATFORM_OFF, 2),
            "mac_count 9": patched_at(SPEC_IMAGE, m.MAC_COUNT_OFF, 9),
        }
        for text, img in cases.items():
            with self.subTest(text):
                self.eeprom(img)
                with self.assertRaisesRegex(ValueError, text):
                    m.read_eeprom()

    def test_slot_checks(self):
        b = patched_at(SPEC_IMAGE, m.MAC_COUNT_OFF, 3)
        self.assertEqual(m.get_mac(b, 3), "02:9e:e6:97:4d:62")
        with self.assertRaisesRegex(ValueError, "not programmed"):
            m.get_mac(b, 4)
        with self.assertRaisesRegex(ValueError, "out of range"):
            m.get_mac(b, 9)
        blank = patched_raw(SPEC_IMAGE, m.MAC_BASE_OFF + 6, b"\x00" * 6)
        with self.assertRaisesRegex(ValueError, "blank"):
            m.get_mac(blank, 2)
        mcast = patched_raw(SPEC_IMAGE, m.MAC_BASE_OFF + 6, b"\x03" + SPEC_IMAGE[m.MAC_BASE_OFF + 7:m.MAC_BASE_OFF + 12])
        with self.assertRaisesRegex(ValueError, "multicast"):
            m.get_mac(mcast, 2)
        dup = patched_raw(SPEC_IMAGE, m.MAC_BASE_OFF + 6, SPEC_IMAGE[m.MAC_BASE_OFF:m.MAC_BASE_OFF + 6])
        with self.assertRaisesRegex(ValueError, "duplicates slot 1"):
            m.get_mac(dup, 2)
        # A duplicate beyond mac_count is not a valid MAC and does not count.
        dup_hidden = patched_at(patched_raw(SPEC_IMAGE, m.MAC_BASE_OFF + 7 * 6, SPEC_IMAGE[m.MAC_BASE_OFF + 6:m.MAC_BASE_OFF + 12]),
                                m.MAC_COUNT_OFF, 7)
        self.assertEqual(m.get_mac(dup_hidden, 2), "02:9e:e6:97:4d:61")


def patched_at(img, off, val):
    b = bytearray(img)
    b[off] = val
    return patched(bytes(b))


def patched_raw(img, off, data):
    b = bytearray(img)
    b[off:off + len(data)] = data
    return patched(bytes(b))


FULL_CONFIG = """
    [mac]
    enabled = true
    set_native_eth_mac = false

    [w5500_spi1]
    enabled = false
    mac_slot = 2

    [w5500_spi2]
    enabled = true
    mac_slot = 3

    [usb_eth_1]
    enabled = true
    controller = ff440000.usb ff450000.usb
    port = 1.1
    mac_slot = 4

    [usb_eth_2]
    enabled = true
    controller = ff440000.usb, ff450000.usb
    port = 1.2
    mac_slot = 5

    [usb_eth_3]
    enabled = true
    controller = ff400000.usb
    port = 1.1
    mac_slot = 6
"""


class TestConfig(TmpDir):
    def test_full(self):
        self.config(FULL_CONFIG)
        enabled, rules, warnings = m.load_config()
        self.assertTrue(enabled)
        self.assertEqual(warnings, [])
        self.assertEqual([(r.section, r.slot) for r in rules],
                         [("w5500_spi2", 3), ("usb_eth_1", 4), ("usb_eth_2", 5), ("usb_eth_3", 6)])
        self.assertEqual(rules[1].controllers, ("ff440000.usb", "ff450000.usb"))
        self.assertEqual(rules[2].controllers, ("ff440000.usb", "ff450000.usb"))
        self.assertEqual(rules[1].driver, "smsc95xx")

    def test_missing_file(self):
        self.assertEqual(m.load_config(), (False, [], []))

    def test_errors(self):
        cases = {
            "used by both \\[w5500_spi2\\] and \\[usb_eth_1\\]":
                "[w5500_spi2]\nenabled = true\n[usb_eth_1]\nenabled = true\ncontroller = a\nport = 1\nmac_slot = 3\n",
            "used by both \\[native\\] and \\[usb_eth_1\\]":
                "[mac]\nset_native_eth_mac = true\n[usb_eth_1]\nenabled = true\ncontroller = a\nport = 1\nmac_slot = 1\n",
            "\\[usb_eth_1\\] missing required key 'port'":
                "[usb_eth_1]\nenabled = true\ncontroller = a\nmac_slot = 4\n",
            "\\[usb_eth_1\\] missing required key 'mac_slot'":
                "[usb_eth_1]\nenabled = true\ncontroller = a\nport = 1\n",
            "mac_slot 9 out of range":
                "[w5500_spi1]\nenabled = true\nmac_slot = 9\n",
            "mac_slot = 'x' is not a number":
                "[w5500_spi1]\nenabled = true\nmac_slot = x\n",
            "enabled = 'maybe' is not a boolean":
                "[w5500_spi1]\nenabled = maybe\n",
            "both match port 1.1 on ff450000.usb":
                "[usb_eth_1]\nenabled = true\ncontroller = ff440000.usb ff450000.usb\nport = 1.1\nmac_slot = 4\n"
                "[usb_eth_2]\nenabled = true\ncontroller = ff450000.usb\nport = 1.1\nmac_slot = 5\n",
            "Parsing|parsing|section header":
                "enabled = true\n",
        }
        for text, cfg in cases.items():
            with self.subTest(text):
                self.config(cfg)
                with self.assertRaisesRegex(m.ConfigError, text):
                    m.load_config()

    def test_disabled_sections_not_validated(self):
        self.config("[usb_eth_1]\nenabled = false\n[w5500_spi1]\nmac_slot = 3\n[w5500_spi2]\nenabled = true\n")
        self.assertEqual([(r.section, r.slot) for r in m.load_config()[1]], [("w5500_spi2", 3)])

    def test_unknown_section_warning(self):
        self.config("[usb_eth1]\nenabled = true\n")
        self.assertEqual(m.load_config()[2], ["config: unknown section [usb_eth1] ignored"])


class TestClassify(TmpDir):
    def setUp(self):
        super().setUp()
        self.config(FULL_CONFIG)
        self.rules = m.load_config()[1]

    def section(self, iface):
        r = m.classify(iface, self.rules)
        return r and r.section

    def test_usb(self):
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx")
        self.iface("eth2", f"{USB_EHCI}/1-1.2/1-1.2:1.0", "smsc95xx")
        self.iface("eth3", f"{USB_OTG}/2-1.1/2-1.1:1.0", "smsc95xx")
        self.iface("eth4", "devices/platform/ff450000.usb/usb3/3-1/3-1.1/3-1.1:1.0", "smsc95xx")
        self.assertEqual([self.section(i) for i in ("eth1", "eth2", "eth3", "eth4")],
                         ["usb_eth_1", "usb_eth_2", "usb_eth_3", "usb_eth_1"])

    def test_usb_not_matched(self):
        self.iface("hub", f"{USB_EHCI}/1-1.1/1-1.1.3/1-1.1.3:1.0", "smsc95xx")
        self.iface("drv", f"{USB_EHCI}/1-1.2/1-1.2:1.0", "r8152")
        self.iface("port", f"{USB_EHCI}/1-1.4/1-1.4:1.0", "smsc95xx")
        self.virtual("br0")
        for i in ("hub", "drv", "port", "br0"):
            with self.subTest(i):
                self.assertIsNone(self.section(i))

    def test_w5500(self):
        self.iface("eth5", "devices/platform/ff4b0000.spi/spi_master/spi2/spi2.0", "w5100", "spi:w5500")
        self.iface("eth6", "devices/platform/ff4a0000.spi/spi_master/spi1/spi1.0", "w5100", "spi:w5500")
        self.assertEqual(self.section("eth5"), "w5500_spi2")
        self.assertIsNone(self.section("eth6"))		# w5500_spi1 disabled

    def test_w5500_controller_pin(self):
        self.config("[w5500_spi2]\nenabled = true\ncontroller = ff4c0000.spi\n")
        self.rules = m.load_config()[1]
        self.iface("eth5", "devices/platform/ff4b0000.spi/spi_master/spi2/spi2.0", "w5100", "spi:w5500")
        self.assertIsNone(self.section("eth5"))

    def test_native(self):
        self.iface("eth0", "devices/platform/ff4e0000.ethernet", "rk_gmac-dwmac")
        self.assertIsNone(self.section("eth0"))
        self.config("[mac]\nset_native_eth_mac = true\n")
        self.rules = m.load_config()[1]
        self.assertEqual(self.section("eth0"), "native")


class TestApply(TmpDir):
    def setUp(self):
        super().setUp()
        self.config(FULL_CONFIG)
        self.rules = m.load_config()[1]
        self.eeprom(SPEC_IMAGE)
        self._ip = m.ip

    def tearDown(self):
        m.ip = self._ip
        super().tearDown()

    def mac(self, iface):
        return m.current_mac(iface)

    def test_set_and_already(self):
        m.ip = FakeIp()
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx")
        self.assertEqual(m.handle("eth1", self.rules, m.read_eeprom), m.EXIT_OK)
        self.assertEqual(self.mac("eth1"), "02:9e:e6:97:4d:63")
        self.assertEqual(m.ip.calls, [("link", "set", "dev", "eth1", "address", "02:9e:e6:97:4d:63")])
        self.assertEqual(m.apply_mac("eth1", "02:9e:e6:97:4d:63"), "ALREADY")

    def test_up_interface_cycled(self):
        m.ip = FakeIp()
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx", up=True)
        self.assertEqual(m.apply_mac("eth1", "02:00:00:00:00:01"), "OK")
        self.assertEqual([c[-1] for c in m.ip.calls], ["down", "02:00:00:00:00:01", "up"])

    def test_left_down_reported(self):
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx", up=True)
        m.ip = FakeIp(fail={"up"})
        with self.assertRaisesRegex(m.ApplyError, "MAC set to .* but interface left DOWN"):
            m.apply_mac("eth1", "02:00:00:00:00:01")
        pathlib.Path(m.SYS_CLASS_NET, "eth1", "address").write_text("aa:bb:cc:dd:ee:01\n")
        m.ip = FakeIp(fail={"up", "address"})
        with self.assertRaisesRegex(m.ApplyError, "MAC unchanged; interface left DOWN"):
            m.apply_mac("eth1", "02:00:00:00:00:01")

    def test_deferred_without_eeprom(self):
        m.ip = FakeIp()
        os.unlink(m.EEPROM_PATH)
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx")
        self.assertEqual(m.handle("eth1", self.rules, m.read_eeprom), m.EXIT_DEFERRED)
        self.assertEqual(m.ip.calls, [])

    def test_nomatch_does_not_read_eeprom(self):
        self.iface("eth9", f"{USB_EHCI}/1-1.4/1-1.4:1.0", "smsc95xx")

        def boom():
            raise AssertionError("EEPROM read for unmanaged interface")
        self.assertEqual(m.handle("eth9", self.rules, boom), m.EXIT_NOMATCH)

    def test_all(self):
        m.ip = FakeIp()
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx")
        self.iface("eth3", f"{USB_OTG}/2-1.1/2-1.1:1.0", "smsc95xx")
        self.iface("eth5", "devices/platform/ff4b0000.spi/spi_master/spi2/spi2.0", "w5100", "spi:w5500")
        self.virtual("br0")
        self.assertEqual(m.run_all(self.rules), m.EXIT_OK)
        self.assertEqual([self.mac(i) for i in ("eth1", "eth3", "eth5")],
                         ["02:9e:e6:97:4d:63", "02:9e:e6:97:4d:65", "02:9e:e6:97:4d:62"])

    def test_all_counts_errors(self):
        m.ip = FakeIp()
        self.eeprom(patched_at(SPEC_IMAGE, m.MAC_COUNT_OFF, 5))
        self.iface("eth1", f"{USB_EHCI}/1-1.1/1-1.1:1.0", "smsc95xx")
        self.iface("eth3", f"{USB_OTG}/2-1.1/2-1.1:1.0", "smsc95xx")	# slot 6 > mac_count
        self.assertEqual(m.run_all(self.rules), m.EXIT_ERROR)
        self.assertEqual(self.mac("eth1"), "02:9e:e6:97:4d:63")
        self.assertEqual(self.mac("eth3"), "aa:bb:cc:dd:ee:01")

    def test_main_config_error(self):
        self.config("[w5500_spi1]\nenabled = true\nmac_slot = 0\n")
        self.assertEqual(m.main(["napi-set-mac", "eth1"]), m.EXIT_ERROR)


if __name__ == "__main__":
    unittest.main()
