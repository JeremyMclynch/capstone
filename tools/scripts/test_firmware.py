#!/usr/bin/env python3
"""
Firmware Integration Test Suite — automated verification over live hardware.

Exercises all major UCI commands and firmware features via the binary protocol,
using either CoAP (over Thread) or UART transport. Reuses protocol classes
from uwb_tool.py — no duplication.

Usage:
    python3 test_firmware.py                          # auto-discover, run all
    python3 test_firmware.py --discover               # just print discovered devices
    python3 test_firmware.py -k TestInfo              # run one test class
    python3 test_firmware.py -k test_stop_idempotent  # run one test method
    python3 test_firmware.py --transport uart          # use UART instead of CoAP
    python3 test_firmware.py --anchor-ip <ipv6>       # explicit anchor IP
    python3 test_firmware.py --tag-ip <ipv6>          # explicit tag IP
    python3 test_firmware.py --anchor-port /dev/ttyACM1  # explicit anchor UART
    python3 test_firmware.py --tag-port /dev/ttyACM3     # explicit tag UART
    python3 test_firmware.py -v                       # verbose output

Requirements:
    - Devices powered on, firmware flashed, Thread network active
    - For CoAP: thread_dongle_setup.sh has been run, wpan0 is up
    - For UART: correct serial ports (check with nrfutil device list)
    - pip install aiocoap pyserial
"""

import argparse
import os
import struct
import subprocess
import sys
import time
import unittest
from dataclasses import dataclass, field
from typing import Optional

# ── Import protocol classes from sibling uwb_tool.py ─────────────────

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uwb_tool import (
    UWBDeviceBase,
    UWBDevice,
    UWBDeviceCoAP,
    CMD_GET_INFO,
    CMD_SET_ROLE,
    CMD_SET_ADDR,
    CMD_SET_INTERVAL,
    CMD_SET_SERVER,
    CMD_START,
    CMD_STOP,
    CMD_GET_STATUS,
    CMD_SAVE_CONFIG,
    CMD_FACTORY_RESET,
    ROLE_ANCHOR,
    ROLE_TAG,
    STATUS_NAMES,
)

# ── UCI status codes (mirror firmware uci.h) ─────────────────────────

STATUS_OK = 0x00
STATUS_ERR_UNKNOWN_CMD = 0x01
STATUS_ERR_BAD_PAYLOAD = 0x02
STATUS_ERR_INVALID_VAL = 0x03
STATUS_ERR_BUSY = 0x04

# ── Known hardware serial numbers (for nrfutil reset) ────────────────

ANCHOR_SERIAL = "1050222631"
TAG_SERIAL = "760206311"

# ── Default board config values (from Kconfig + board .conf) ─────────

DEFAULT_ANCHOR_ADDR = 0x0001
DEFAULT_TAG_ADDR = 0x0100
DEFAULT_INTERVAL_MS = 1000  # Kconfig default (boards may override)
THREAD_IFACE = "wpan0"


# ── Test configuration ───────────────────────────────────────────────

@dataclass
class TestConfig:
    """Runtime configuration injected into all test classes."""
    transport: str = "coap"  # "coap" or "uart"
    anchor_ip: Optional[str] = None
    tag_ip: Optional[str] = None
    anchor_port: str = "/dev/ttyACM1"
    tag_port: str = "/dev/ttyACM3"
    thread_iface: str = THREAD_IFACE
    anchor_serial: str = ANCHOR_SERIAL
    tag_serial: str = TAG_SERIAL
    # Populated by auto-discovery
    discovered_ips: list = field(default_factory=list)


# ── Discovery helpers ────────────────────────────────────────────────

def get_host_ips(iface: str) -> set:
    """Return set of IPv6 addresses assigned to the given interface."""
    try:
        result = subprocess.run(
            ["ip", "-6", "addr", "show", "dev", iface],
            capture_output=True, text=True, timeout=5,
        )
        ips = set()
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith("inet6 "):
                addr = line.split()[1].split("/")[0]
                ips.add(addr)
        return ips
    except Exception:
        return set()


def discover_devices(iface: str, timeout: int = 5) -> list:
    """Ping ff03::1 on the Thread interface and collect responding IPs."""
    host_ips = get_host_ips(iface)
    try:
        result = subprocess.run(
            ["ping", "-6", "-c", str(timeout), "-I", iface, "ff03::1"],
            capture_output=True, text=True, timeout=timeout + 3,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return []

    ips = set()
    for line in result.stdout.splitlines():
        # "64 bytes from fdde:ad00:beef:0:xxxx: icmp_seq=..."
        if "bytes from" in line:
            addr = line.split("from ")[1].split(":")[:-1]
            addr = ":".join(addr)
            # Remove trailing scope ID if present
            addr = addr.split("%")[0]
            if addr not in host_ips:
                ips.add(addr)

    return sorted(ips)


def identify_device(ip: str) -> Optional[dict]:
    """Send GET_INFO via CoAP and return parsed info, or None on failure."""
    try:
        dev = UWBDeviceCoAP(ip, timeout=3.0)
        info = dev.get_info()
        if "error" in info:
            return None
        return info
    except Exception:
        return None


def auto_discover_and_identify(config: TestConfig) -> bool:
    """Discover devices via multicast, identify anchor/tag, populate config.

    Returns True if both anchor and tag were found.
    """
    print(f"\n  Discovering devices on {config.thread_iface}...")
    ips = discover_devices(config.thread_iface)
    config.discovered_ips = ips

    if not ips:
        print("  No devices responded to multicast ping.")
        return False

    print(f"  Found {len(ips)} device(s): {ips}")

    for ip in ips:
        info = identify_device(ip)
        if info is None:
            print(f"  {ip}: no UCI response")
            continue
        role = info.get("role", "?")
        addr = info.get("addr", "?")
        print(f"  {ip}: role={role} addr={addr}")
        if role == "ANCHOR" and config.anchor_ip is None:
            config.anchor_ip = ip
        elif role == "TAG" and config.tag_ip is None:
            config.tag_ip = ip

    found = config.anchor_ip is not None and config.tag_ip is not None
    if found:
        print(f"  Anchor: {config.anchor_ip}")
        print(f"  Tag:    {config.tag_ip}")
    else:
        if config.anchor_ip is None:
            print("  WARNING: No anchor found")
        if config.tag_ip is None:
            print("  WARNING: No tag found")

    return found


# ── Device helpers ───────────────────────────────────────────────────

def reset_device(serial_number: str):
    """Reset a device via nrfutil (J-Link)."""
    subprocess.run(
        ["nrfutil", "device", "reset", "--serial-number", serial_number],
        capture_output=True, timeout=10,
    )


def wait_for_coap_ready(ip: str, timeout: float = 30.0) -> bool:
    """Poll GET_INFO via CoAP until the device responds or timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = identify_device(ip)
        if info is not None:
            return True
        time.sleep(1.0)
    return False


def wait_for_uart_ready(port: str, timeout: float = 15.0) -> Optional[UWBDevice]:
    """Try to open serial port and GET_INFO until success or timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            dev = UWBDevice(port, timeout=2.0)
            info = dev.get_info()
            if "error" not in info:
                return dev
            dev.close()
        except Exception:
            pass
        time.sleep(1.0)
    return None


def make_device(config: TestConfig, role: str) -> UWBDeviceBase:
    """Create a device connection for the given role ('anchor' or 'tag')."""
    if config.transport == "coap":
        ip = config.anchor_ip if role == "anchor" else config.tag_ip
        if ip is None:
            raise unittest.SkipTest(f"No {role} IP configured (use --{role}-ip or auto-discover)")
        return UWBDeviceCoAP(ip, timeout=5.0)
    else:
        port = config.anchor_port if role == "anchor" else config.tag_port
        return UWBDevice(port, timeout=2.0)


def ensure_factory_defaults(config: TestConfig, role: str):
    """Factory-reset a device and wait for it to come back with defaults."""
    dev = make_device(config, role)
    try:
        dev.factory_reset()
    except Exception:
        pass  # Device reboots before responding — expected
    finally:
        dev.close()

    serial = config.anchor_serial if role == "anchor" else config.tag_serial

    # Wait for device to reboot and rejoin Thread
    time.sleep(3.0)

    if config.transport == "coap":
        ip = config.anchor_ip if role == "anchor" else config.tag_ip
        if not wait_for_coap_ready(ip, timeout=30.0):
            raise RuntimeError(f"Device {role} did not rejoin Thread after factory reset")
    else:
        port = config.anchor_port if role == "anchor" else config.tag_port
        dev = wait_for_uart_ready(port, timeout=15.0)
        if dev is None:
            raise RuntimeError(f"Device {role} UART not ready after factory reset")
        dev.close()


# ── Base test class ──────────────────────────────────────────────────

class FirmwareTestCase(unittest.TestCase):
    """Base class that provides access to test config and device helpers."""

    _config: TestConfig = None  # Injected before test run

    def make_device(self, role: str) -> UWBDeviceBase:
        return make_device(self._config, role)

    def make_anchor(self) -> UWBDeviceBase:
        return self.make_device("anchor")

    def make_tag(self) -> UWBDeviceBase:
        return self.make_device("tag")


# ═══════════════════════════════════════════════════════════════════════
# Test Class 1: Discovery (CoAP only)
# ═══════════════════════════════════════════════════════════════════════

class TestDiscovery(FirmwareTestCase):
    """Verify Thread multicast discovery finds devices."""

    def setUp(self):
        if self._config.transport != "coap":
            self.skipTest("Discovery tests require CoAP transport")

    def test_multicast_ping_finds_devices(self):
        """At least one device responds to ff03::1 ping."""
        ips = discover_devices(self._config.thread_iface)
        self.assertGreater(len(ips), 0, "No devices responded to multicast ping")

    def test_anchor_reachable(self):
        """Anchor responds to GET_INFO over CoAP."""
        self.assertIsNotNone(self._config.anchor_ip, "No anchor IP")
        info = identify_device(self._config.anchor_ip)
        self.assertIsNotNone(info, "Anchor did not respond to GET_INFO")
        self.assertEqual(info["role"], "ANCHOR")

    def test_tag_reachable(self):
        """Tag responds to GET_INFO over CoAP."""
        self.assertIsNotNone(self._config.tag_ip, "No tag IP")
        info = identify_device(self._config.tag_ip)
        self.assertIsNotNone(info, "Tag did not respond to GET_INFO")
        self.assertEqual(info["role"], "TAG")


# ═══════════════════════════════════════════════════════════════════════
# Test Class 2: Info
# ═══════════════════════════════════════════════════════════════════════

class TestInfo(FirmwareTestCase):
    """Verify GET_INFO returns correct fields for both devices."""

    def test_anchor_info_fields(self):
        """Anchor GET_INFO returns all expected fields."""
        dev = self.make_anchor()
        try:
            info = dev.get_info()
        finally:
            dev.close()
        self.assertNotIn("error", info, f"GET_INFO failed: {info}")
        self.assertIn("fw_version", info)
        self.assertIn("role", info)
        self.assertIn("addr", info)
        self.assertIn("running", info)

    def test_tag_info_fields(self):
        """Tag GET_INFO returns all expected fields."""
        dev = self.make_tag()
        try:
            info = dev.get_info()
        finally:
            dev.close()
        self.assertNotIn("error", info, f"GET_INFO failed: {info}")
        self.assertIn("fw_version", info)
        self.assertIn("role", info)
        self.assertIn("addr", info)
        self.assertIn("running", info)

    def test_anchor_role(self):
        """Anchor reports role as ANCHOR."""
        dev = self.make_anchor()
        try:
            info = dev.get_info()
        finally:
            dev.close()
        self.assertEqual(info["role"], "ANCHOR")

    def test_tag_role(self):
        """Tag reports role as TAG."""
        dev = self.make_tag()
        try:
            info = dev.get_info()
        finally:
            dev.close()
        self.assertEqual(info["role"], "TAG")

    def test_anchor_fw_version(self):
        """Anchor firmware version is parseable (major.minor)."""
        dev = self.make_anchor()
        try:
            info = dev.get_info()
        finally:
            dev.close()
        parts = info["fw_version"].split(".")
        self.assertEqual(len(parts), 2)
        int(parts[0])  # Should not raise
        int(parts[1])

    def test_info_payload_length(self):
        """GET_INFO raw response has exactly 6 bytes payload."""
        dev = self.make_anchor()
        try:
            status, payload = dev._send_request(CMD_GET_INFO)
        finally:
            dev.close()
        self.assertEqual(status, STATUS_OK)
        self.assertEqual(len(payload), 6)


# ═══════════════════════════════════════════════════════════════════════
# Test Class 3: Status
# ═══════════════════════════════════════════════════════════════════════

class TestStatus(FirmwareTestCase):
    """Verify GET_STATUS returns plausible values."""

    def test_anchor_status_fields(self):
        """Anchor GET_STATUS returns all expected fields."""
        dev = self.make_anchor()
        try:
            st = dev.get_status()
        finally:
            dev.close()
        self.assertNotIn("error", st, f"GET_STATUS failed: {st}")
        self.assertIn("running", st)
        self.assertIn("uptime_s", st)
        self.assertIn("range_count", st)
        self.assertIn("last_distance_m", st)

    def test_uptime_positive(self):
        """Device uptime is greater than zero."""
        dev = self.make_anchor()
        try:
            st = dev.get_status()
        finally:
            dev.close()
        self.assertGreater(st["uptime_s"], 0, "Uptime should be > 0")

    def test_status_payload_length(self):
        """GET_STATUS raw response has exactly 13 bytes payload."""
        dev = self.make_anchor()
        try:
            status, payload = dev._send_request(CMD_GET_STATUS)
        finally:
            dev.close()
        self.assertEqual(status, STATUS_OK)
        self.assertEqual(len(payload), 13)


# ═══════════════════════════════════════════════════════════════════════
# Test Class 4: Start/Stop
# ═══════════════════════════════════════════════════════════════════════

class TestStartStop(FirmwareTestCase):
    """Verify start/stop ranging control (restores original state)."""

    def _get_running(self, dev: UWBDeviceBase) -> bool:
        st = dev.get_status()
        self.assertNotIn("error", st)
        return st["running"]

    def test_stop_then_verify(self):
        """Stop ranging, verify device reports not running, then restart."""
        dev = self.make_anchor()
        try:
            was_running = self._get_running(dev)
            result = dev.stop()
            self.assertEqual(result, "OK")
            time.sleep(0.3)
            self.assertFalse(self._get_running(dev), "Device should be stopped")
        finally:
            # Restore
            if was_running:
                dev.start()
                time.sleep(0.3)
            dev.close()

    def test_start_then_verify(self):
        """Start ranging from stopped state, verify running."""
        dev = self.make_anchor()
        try:
            was_running = self._get_running(dev)
            # Ensure stopped first
            dev.stop()
            time.sleep(0.3)
            result = dev.start()
            self.assertEqual(result, "OK")
            time.sleep(0.3)
            self.assertTrue(self._get_running(dev), "Device should be running")
        finally:
            # Restore original state
            if not was_running:
                dev.stop()
                time.sleep(0.3)
            dev.close()

    def test_double_start_returns_busy(self):
        """Starting an already-running device returns ERR_BUSY."""
        dev = self.make_anchor()
        try:
            was_running = self._get_running(dev)
            if not was_running:
                dev.start()
                time.sleep(0.3)
            # Now it's running — second start should fail
            status, _ = dev._send_request(CMD_START)
            self.assertEqual(status, STATUS_ERR_BUSY)
        finally:
            if not was_running:
                dev.stop()
                time.sleep(0.3)
            dev.close()

    def test_stop_is_idempotent(self):
        """Stopping an already-stopped device returns OK."""
        dev = self.make_anchor()
        try:
            was_running = self._get_running(dev)
            dev.stop()
            time.sleep(0.3)
            # Second stop
            result = dev.stop()
            self.assertEqual(result, "OK")
        finally:
            if was_running:
                dev.start()
                time.sleep(0.3)
            dev.close()

    def test_stop_start_cycle(self):
        """Full stop→start cycle leaves device in running state."""
        dev = self.make_anchor()
        try:
            was_running = self._get_running(dev)
            dev.stop()
            time.sleep(0.5)
            self.assertFalse(self._get_running(dev))
            dev.start()
            time.sleep(0.5)
            self.assertTrue(self._get_running(dev))
        finally:
            if not was_running:
                dev.stop()
                time.sleep(0.3)
            dev.close()


# ═══════════════════════════════════════════════════════════════════════
# Test Class 5: Input Validation
# ═══════════════════════════════════════════════════════════════════════

class TestInputValidation(FirmwareTestCase):
    """Verify the firmware rejects invalid commands and payloads."""

    def test_unknown_command(self):
        """Unknown command byte returns ERR_UNKNOWN_CMD."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(0xFF)
            self.assertEqual(status, STATUS_ERR_UNKNOWN_CMD)
        finally:
            dev.close()

    def test_set_role_empty_payload(self):
        """SET_ROLE with no payload returns ERR_BAD_PAYLOAD."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_ROLE, b"")
            self.assertEqual(status, STATUS_ERR_BAD_PAYLOAD)
        finally:
            dev.close()

    def test_set_role_oversized_payload(self):
        """SET_ROLE with 2-byte payload returns ERR_BAD_PAYLOAD."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_ROLE, b"\x00\x00")
            self.assertEqual(status, STATUS_ERR_BAD_PAYLOAD)
        finally:
            dev.close()

    def test_set_role_invalid_value(self):
        """SET_ROLE with value 0x05 returns ERR_INVALID_VALUE."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_ROLE, bytes([0x05]))
            self.assertEqual(status, STATUS_ERR_INVALID_VAL)
        finally:
            dev.close()

    def test_set_addr_empty_payload(self):
        """SET_ADDR with no payload returns ERR_BAD_PAYLOAD."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_ADDR, b"")
            self.assertEqual(status, STATUS_ERR_BAD_PAYLOAD)
        finally:
            dev.close()

    def test_set_addr_zero(self):
        """SET_ADDR with 0x0000 returns ERR_INVALID_VALUE."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_ADDR, struct.pack("<H", 0x0000))
            self.assertEqual(status, STATUS_ERR_INVALID_VAL)
        finally:
            dev.close()

    def test_set_addr_ffff(self):
        """SET_ADDR with 0xFFFF returns ERR_INVALID_VALUE."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_ADDR, struct.pack("<H", 0xFFFF))
            self.assertEqual(status, STATUS_ERR_INVALID_VAL)
        finally:
            dev.close()

    def test_set_interval_too_low(self):
        """SET_INTERVAL with 10ms (below 50ms min) returns ERR_INVALID_VALUE."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_INTERVAL, struct.pack("<H", 10))
            self.assertEqual(status, STATUS_ERR_INVALID_VAL)
        finally:
            dev.close()

    def test_set_interval_too_high(self):
        """SET_INTERVAL with 20000ms (above 10000ms max) returns ERR_INVALID_VALUE."""
        dev = self.make_anchor()
        try:
            status, _ = dev._send_request(CMD_SET_INTERVAL, struct.pack("<H", 20000))
            self.assertEqual(status, STATUS_ERR_INVALID_VAL)
        finally:
            dev.close()


# ═══════════════════════════════════════════════════════════════════════
# Test Class 6: Interval Change
# ═══════════════════════════════════════════════════════════════════════

class TestIntervalChange(FirmwareTestCase):
    """Verify SET_INTERVAL takes effect on live ranging (restores original)."""

    def _measure_rate(self, dev: UWBDeviceBase, duration: float = 3.0) -> float:
        """Measure ranging rate (ranges/sec) by polling range_count."""
        st1 = dev.get_status()
        self.assertNotIn("error", st1)
        count1 = st1["range_count"]
        time.sleep(duration)
        st2 = dev.get_status()
        self.assertNotIn("error", st2)
        count2 = st2["range_count"]
        return (count2 - count1) / duration

    def test_interval_change_affects_rate(self):
        """Changing tag interval from slow to fast measurably increases range rate.

        The tag is the DS-TWR initiator — its ranging_interval_ms controls the
        pacing loop. The anchor is the responder and has no inter-cycle sleep,
        so we measure on the tag.
        """
        tag = self.make_tag()
        try:
            was_running = tag.get_status().get("running", False)
            if not was_running:
                tag.start()
                time.sleep(0.5)

            # Set slow interval (2s between ranges)
            result = tag.set_interval(2000)
            self.assertEqual(result, "OK")
            time.sleep(1.0)  # let new interval take effect
            slow_rate = self._measure_rate(tag, duration=6.0)

            # Set fast interval (200ms between ranges)
            result = tag.set_interval(200)
            self.assertEqual(result, "OK")
            time.sleep(1.0)
            fast_rate = self._measure_rate(tag, duration=4.0)

            # Fast rate should be noticeably higher than slow rate
            self.assertGreater(fast_rate, slow_rate * 1.5,
                               f"Fast rate ({fast_rate:.1f}/s) should be much higher "
                               f"than slow rate ({slow_rate:.1f}/s)")
        finally:
            tag.set_interval(DEFAULT_INTERVAL_MS)
            if not was_running:
                tag.stop()
                time.sleep(0.3)
            tag.close()

    def test_interval_boundary_values(self):
        """SET_INTERVAL accepts min (50ms) and max (10000ms) boundary values."""
        dev = self.make_anchor()
        try:
            result = dev.set_interval(50)
            self.assertEqual(result, "OK")
            result = dev.set_interval(10000)
            self.assertEqual(result, "OK")
        finally:
            dev.set_interval(DEFAULT_INTERVAL_MS)
            dev.close()


# ═══════════════════════════════════════════════════════════════════════
# Test Class 7: Ranging
# ═══════════════════════════════════════════════════════════════════════

class TestRanging(FirmwareTestCase):
    """Verify active ranging produces incrementing counts and plausible distance."""

    def test_anchor_range_count_increments(self):
        """Anchor range_count increases over time when running."""
        dev = self.make_anchor()
        try:
            was_running = dev.get_status().get("running", False)
            if not was_running:
                dev.start()
                time.sleep(0.5)

            st1 = dev.get_status()
            self.assertNotIn("error", st1)
            time.sleep(3.0)
            st2 = dev.get_status()
            self.assertNotIn("error", st2)

            self.assertGreater(st2["range_count"], st1["range_count"],
                               "Anchor range_count should increment")
        finally:
            if not was_running:
                dev.stop()
                time.sleep(0.3)
            dev.close()

    def test_tag_range_count_increments(self):
        """Tag range_count increases over time when running."""
        dev = self.make_tag()
        try:
            was_running = dev.get_status().get("running", False)
            if not was_running:
                dev.start()
                time.sleep(0.5)

            st1 = dev.get_status()
            self.assertNotIn("error", st1)
            time.sleep(3.0)
            st2 = dev.get_status()
            self.assertNotIn("error", st2)

            self.assertGreater(st2["range_count"], st1["range_count"],
                               "Tag range_count should increment")
        finally:
            if not was_running:
                dev.stop()
                time.sleep(0.3)
            dev.close()

    def test_distance_plausible(self):
        """Anchor last_distance_m is between 0 and 200m when ranging."""
        dev = self.make_anchor()
        try:
            was_running = dev.get_status().get("running", False)
            if not was_running:
                dev.start()
                time.sleep(1.0)

            # Wait for at least one measurement
            time.sleep(2.0)
            st = dev.get_status()
            self.assertNotIn("error", st)

            if st["range_count"] > 0:
                dist = st["last_distance_m"]
                self.assertGreater(dist, 0.0,
                                   "Distance should be > 0 with devices in proximity")
                self.assertLess(dist, 200.0,
                                "Distance should be < 200m for indoor setup")
            else:
                self.skipTest("No ranges completed — check hardware placement")
        finally:
            if not was_running:
                dev.stop()
                time.sleep(0.3)
            dev.close()


# ═══════════════════════════════════════════════════════════════════════
# Test Class 8: Config Persistence (DESTRUCTIVE — factory-resets in tearDown)
# ═══════════════════════════════════════════════════════════════════════

class TestConfigPersistence(FirmwareTestCase):
    """Verify config save/load across reboots. Factory-resets in tearDown."""

    def tearDown(self):
        """Always restore factory defaults after each test."""
        try:
            ensure_factory_defaults(self._config, "anchor")
        except Exception as e:
            print(f"\n  WARNING: tearDown factory reset failed: {e}")

    def _reboot_and_wait(self, role: str) -> UWBDeviceBase:
        """Reboot device via nrfutil and wait for it to come back."""
        serial = self._config.anchor_serial if role == "anchor" else self._config.tag_serial
        reset_device(serial)
        time.sleep(3.0)

        if self._config.transport == "coap":
            ip = self._config.anchor_ip if role == "anchor" else self._config.tag_ip
            ok = wait_for_coap_ready(ip, timeout=30.0)
            self.assertTrue(ok, f"Device {role} did not rejoin after reboot")
            return UWBDeviceCoAP(ip, timeout=5.0)
        else:
            port = self._config.anchor_port if role == "anchor" else self._config.tag_port
            dev = wait_for_uart_ready(port, timeout=15.0)
            self.assertIsNotNone(dev, f"Device {role} UART not ready after reboot")
            return dev

    def test_save_persists_across_reboot(self):
        """SET_ADDR + SAVE persists after reboot."""
        test_addr = 0x0042
        dev = self.make_anchor()
        try:
            result = dev.set_addr(test_addr)
            self.assertEqual(result, "OK")
            result = dev.save()
            self.assertEqual(result, "OK")
        finally:
            dev.close()

        # Reboot and check
        dev = self._reboot_and_wait("anchor")
        try:
            info = dev.get_info()
            self.assertNotIn("error", info)
            self.assertEqual(info["addr"], f"0x{test_addr:04X}",
                             "Saved address should persist across reboot")
        finally:
            dev.close()

    def test_no_save_does_not_persist(self):
        """SET_ADDR without SAVE does NOT persist after reboot."""
        dev = self.make_anchor()
        try:
            original_info = dev.get_info()
            original_addr = original_info["addr"]
            # Change addr but do NOT save
            result = dev.set_addr(0x0099)
            self.assertEqual(result, "OK")
        finally:
            dev.close()

        # Reboot and check
        dev = self._reboot_and_wait("anchor")
        try:
            info = dev.get_info()
            self.assertNotIn("error", info)
            self.assertEqual(info["addr"], original_addr,
                             "Unsaved address should revert after reboot")
        finally:
            dev.close()

    def test_factory_reset_restores_defaults(self):
        """Factory reset restores Kconfig default address."""
        dev = self.make_anchor()
        try:
            # Change and save a non-default address
            dev.set_addr(0x0042)
            dev.save()
        finally:
            dev.close()

        # Factory reset (reboots device)
        ensure_factory_defaults(self._config, "anchor")

        dev = self.make_anchor()
        try:
            info = dev.get_info()
            self.assertNotIn("error", info)
            self.assertEqual(info["addr"], f"0x{DEFAULT_ANCHOR_ADDR:04X}",
                             "Factory reset should restore default address")
            self.assertEqual(info["role"], "ANCHOR",
                             "Factory reset should restore default role")
        finally:
            dev.close()


# ═══════════════════════════════════════════════════════════════════════
# Test Class 9: Role Swap (DESTRUCTIVE — factory-resets in tearDown)
# ═══════════════════════════════════════════════════════════════════════

class TestRoleSwap(FirmwareTestCase):
    """Full role-swap cycle. Factory-resets both devices in tearDown."""

    def setUp(self):
        if self._config.transport != "coap":
            self.skipTest("Role swap test requires CoAP (needs Thread rejoin)")

    def tearDown(self):
        """Restore both devices to factory defaults."""
        for role in ("anchor", "tag"):
            try:
                ensure_factory_defaults(self._config, role)
            except Exception as e:
                print(f"\n  WARNING: tearDown factory reset ({role}) failed: {e}")

    def test_full_role_swap(self):
        """Stop both → swap roles → save → reboot → verify swapped roles."""
        anchor_dev = self.make_anchor()
        tag_dev = self.make_tag()

        try:
            # Stop both
            anchor_dev.stop()
            tag_dev.stop()
            time.sleep(0.5)

            # Swap roles
            result = anchor_dev.set_role("tag")
            self.assertEqual(result, "OK")
            result = tag_dev.set_role("anchor")
            self.assertEqual(result, "OK")

            # Save both
            result = anchor_dev.save()
            self.assertEqual(result, "OK")
            result = tag_dev.save()
            self.assertEqual(result, "OK")
        finally:
            anchor_dev.close()
            tag_dev.close()

        # Reboot both
        reset_device(self._config.anchor_serial)
        reset_device(self._config.tag_serial)
        time.sleep(3.0)

        # Wait for both to rejoin
        ok = wait_for_coap_ready(self._config.anchor_ip, timeout=30.0)
        self.assertTrue(ok, "Former anchor did not rejoin")
        ok = wait_for_coap_ready(self._config.tag_ip, timeout=30.0)
        self.assertTrue(ok, "Former tag did not rejoin")

        # Verify swapped roles
        anchor_info = identify_device(self._config.anchor_ip)
        self.assertIsNotNone(anchor_info)
        self.assertEqual(anchor_info["role"], "TAG",
                         "Former anchor should now be TAG")

        tag_info = identify_device(self._config.tag_ip)
        self.assertIsNotNone(tag_info)
        self.assertEqual(tag_info["role"], "ANCHOR",
                         "Former tag should now be ANCHOR")


# ═══════════════════════════════════════════════════════════════════════
# Test ordering: non-destructive first, then destructive
# ═══════════════════════════════════════════════════════════════════════

def ordered_suite(config: TestConfig) -> unittest.TestSuite:
    """Build test suite in deterministic order: safe tests first."""
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    # Inject config into all test classes
    test_classes = [
        TestDiscovery,
        TestInfo,
        TestStatus,
        TestStartStop,
        TestInputValidation,
        TestIntervalChange,
        TestRanging,
        TestConfigPersistence,
        TestRoleSwap,
    ]

    for cls in test_classes:
        cls._config = config
        suite.addTests(loader.loadTestsFromTestCase(cls))

    return suite


# ═══════════════════════════════════════════════════════════════════════
# CLI entry point
# ═══════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Firmware integration test suite for UWB Mesh Tracker",
        epilog="Devices must be powered on with Thread network active.",
    )
    parser.add_argument("--transport", choices=["coap", "uart"], default="coap",
                        help="Transport to use (default: coap)")
    parser.add_argument("--anchor-ip", metavar="IPV6",
                        help="Anchor IPv6 address (skips auto-discovery)")
    parser.add_argument("--tag-ip", metavar="IPV6",
                        help="Tag IPv6 address (skips auto-discovery)")
    parser.add_argument("--anchor-port", metavar="PORT", default="/dev/ttyACM1",
                        help="Anchor serial port (default: /dev/ttyACM1)")
    parser.add_argument("--tag-port", metavar="PORT", default="/dev/ttyACM3",
                        help="Tag serial port (default: /dev/ttyACM3)")
    parser.add_argument("--iface", default=THREAD_IFACE,
                        help=f"Thread network interface (default: {THREAD_IFACE})")
    parser.add_argument("--discover", action="store_true",
                        help="Just discover devices and exit (no tests)")
    parser.add_argument("-k", metavar="PATTERN",
                        help="Only run tests matching this pattern")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Verbose test output")

    args = parser.parse_args()

    config = TestConfig(
        transport=args.transport,
        anchor_ip=args.anchor_ip,
        tag_ip=args.tag_ip,
        anchor_port=args.anchor_port,
        tag_port=args.tag_port,
        thread_iface=args.iface,
    )

    # ── Discovery ────────────────────────────────────────────────────

    if args.transport == "coap" and (config.anchor_ip is None or config.tag_ip is None):
        found = auto_discover_and_identify(config)
        if args.discover:
            sys.exit(0 if found else 1)
        if not found:
            print("\nWARNING: Not all devices discovered. Some tests may be skipped.\n")
    elif args.discover:
        if args.transport == "uart":
            print("Discovery requires CoAP transport. Use --transport coap")
            sys.exit(1)
        found = auto_discover_and_identify(config)
        sys.exit(0 if found else 1)

    # ── Build and filter test suite ──────────────────────────────────

    suite = ordered_suite(config)

    if args.k:
        # Filter tests matching the pattern
        filtered = unittest.TestSuite()
        for test in suite:
            if isinstance(test, unittest.TestSuite):
                for t in test:
                    if args.k in str(t):
                        filtered.addTest(t)
            elif args.k in str(test):
                filtered.addTest(test)
        suite = filtered

    # ── Run ──────────────────────────────────────────────────────────

    verbosity = 2 if args.verbose else 1
    runner = unittest.TextTestRunner(verbosity=verbosity)
    result = runner.run(suite)

    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
