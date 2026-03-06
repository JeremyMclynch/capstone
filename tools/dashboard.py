#!/usr/bin/env python3
"""
UWB Dashboard — curses-based TUI combining monitor output, device discovery,
and UCI command execution in a single screen.

Usage:
    python3 tools/dashboard.py                    # default: auto-discover, start monitor
    python3 tools/dashboard.py --no-monitor       # skip monitor subprocess
    python3 tools/dashboard.py --iface wpan0      # Thread interface (default: wpan0)

Layout:
    ┌─────────────────────────────────────────────────────────┐
    │  Monitor Output (top half)                              │
    ├────────────────────────────┬────────────────────────────┤
    │  Commands (bottom-left)    │  Devices (bottom-right)    │
    └────────────────────────────┴────────────────────────────┘

Dependencies: aiocoap, pyserial (same as monitor.py / uwb_tool.py)
"""

import argparse
import collections
import curses
import glob
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# Import UWBDeviceCoAP from sibling scripts/ directory
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "scripts"))
from uwb_tool import UWBDeviceCoAP


# ── Data structures ──────────────────────────────────────────────────

@dataclass
class DeviceInfo:
    ip: str
    role: str = ""           # "ANCHOR" / "TAG"
    addr: str = ""           # "0x0001"
    fw_version: str = ""     # "1.0"
    running: bool = False
    uptime_s: int = 0
    range_count: int = 0
    last_distance_m: float = 0.0
    last_seen: float = 0.0   # time.monotonic()


# UI states
STATE_NORMAL = 0
STATE_SELECT_DEVICE = 1
STATE_INPUT = 2
STATE_EXECUTING = 3
STATE_RESULT = 4
STATE_CONFIRM = 5

COMMANDS = [
    "Get Info",
    "Get Status",
    "Start Ranging",
    "Stop Ranging",
    "Set Interval",
    "Set Address",
    "Set Role",
    "Calibrate",
    "Get Cal Offset",
    "Set Cal Offset",
    "CIR Enable",
    "CIR Disable",
    "Set Discovery Interval",
    "Trigger Discovery",
    "Save Config",
    "Reboot",
    "Factory Reset",
    "Enter Bootloader",
    "Toggle Monitor",
    "Build All Firmware",
    "OTA Upload Firmware",
    "OTA Test Update",
    "OTA Confirm Update",
    "OTA Image List",
]

# Commands that need text input
INPUT_COMMANDS = {"Set Interval", "Set Address", "Set Cal Offset", "CIR Enable",
                  "Set Discovery Interval"}
# Commands that offer a pick list (OTA Upload dynamically populates pick_options)
PICK_COMMANDS = {"Set Role", "OTA Upload Firmware"}
ROLE_OPTIONS = ["anchor", "tag"]

# OTA status markers returned by execute_command to signal state changes
_OTA_PENDING = "OTA_PENDING\n"
_OTA_CONFIRMING = "OTA_CONFIRMING\n"
_OTA_DONE = "OTA_DONE\n"


# ── Monitor subprocess manager ───────────────────────────────────────

class MonitorProcess:
    """Manages monitor.py as a subprocess, collecting its stdout lines."""

    def __init__(self, tools_dir: str):
        self.tools_dir = tools_dir
        self.lines = collections.deque(maxlen=200)
        self.process: Optional[subprocess.Popen] = None
        self._reader_thread: Optional[threading.Thread] = None
        self.distance_only = True
        self._lock = threading.Lock()

    def start(self):
        """Start the monitor subprocess."""
        args = [sys.executable, "-u", os.path.join(self.tools_dir, "monitor.py")]
        if self.distance_only:
            args.append("-d")
        try:
            self.process = subprocess.Popen(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
        except Exception as e:
            self.lines.append(f"[dashboard] Failed to start monitor: {e}")
            return

        self._reader_thread = threading.Thread(
            target=self._read_loop, daemon=True
        )
        self._reader_thread.start()

    def _read_loop(self):
        """Read stdout lines from the monitor process."""
        proc = self.process
        if not proc or not proc.stdout:
            return
        try:
            for line in proc.stdout:
                with self._lock:
                    self.lines.append(_ANSI_RE.sub("", line.rstrip("\n")))
        except Exception:
            pass

    def stop(self):
        """Terminate the monitor subprocess."""
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=3)
            except Exception:
                try:
                    self.process.kill()
                except Exception:
                    pass
            self.process = None

    def restart(self, distance_only: bool):
        """Restart monitor with different flags."""
        self.stop()
        self.distance_only = distance_only
        with self._lock:
            self.lines.clear()
            self.lines.append(
                f"[dashboard] Monitor restarted: "
                f"{'distance only' if distance_only else 'distance + events'}"
            )
        self.start()

    def get_lines(self) -> list:
        """Return a snapshot of buffered lines."""
        with self._lock:
            return list(self.lines)

    @property
    def mode_label(self) -> str:
        return "distance only" if self.distance_only else "distance + events"


# ── Device discovery and status polling ──────────────────────────────

class DeviceTracker:
    """Background thread that discovers devices and polls their status."""

    def __init__(self, iface: str = "wpan0", interval: float = 10.0):
        self.iface = iface
        self.interval = interval
        self.devices: dict[str, DeviceInfo] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.last_scan: float = 0.0
        self.scan_error: str = ""

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    def _run(self):
        while not self._stop.is_set():
            self._scan()
            self._stop.wait(self.interval)

    def _get_host_ips(self) -> set:
        """Return IPv6 addresses assigned to the Thread interface."""
        try:
            result = subprocess.run(
                ["ip", "-6", "addr", "show", "dev", self.iface],
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

    def _discover_ips(self) -> list:
        """Ping multicast and collect responding device IPs."""
        host_ips = self._get_host_ips()
        try:
            result = subprocess.run(
                ["ping6", "-c", "3", "-I", self.iface, "ff03::1"],
                capture_output=True, text=True, timeout=8,
            )
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return []

        ips = set()
        for line in result.stdout.splitlines():
            if "bytes from" in line:
                parts = line.split("from ")[1].split(":")
                addr = ":".join(parts[:-1])
                addr = addr.split("%")[0]
                if addr not in host_ips:
                    ips.add(addr)
        return sorted(ips)

    def _identify(self, ip: str) -> Optional[DeviceInfo]:
        """Query a device for info + status via CoAP."""
        try:
            dev = UWBDeviceCoAP(ip, timeout=3.0)
            info = dev.get_info()
            if "error" in info:
                return None

            di = DeviceInfo(
                ip=ip,
                role=info.get("role", "?"),
                addr=info.get("addr", "?"),
                fw_version=info.get("fw_version", "?"),
                running=info.get("running", False),
                last_seen=time.monotonic(),
            )

            status = dev.get_status()
            if "error" not in status:
                di.uptime_s = status.get("uptime_s", 0)
                di.range_count = status.get("range_count", 0)
                di.last_distance_m = status.get("last_distance_m", 0.0)
                di.running = status.get("running", di.running)

            return di
        except Exception:
            return None

    def _scan(self):
        """Run one discovery + identification cycle."""
        try:
            ips = self._discover_ips()
            self.scan_error = ""
        except Exception as e:
            self.scan_error = str(e)
            self.last_scan = time.monotonic()
            return

        for ip in ips:
            di = self._identify(ip)
            if di:
                with self._lock:
                    self.devices[ip] = di

        self.last_scan = time.monotonic()

    def get_devices(self) -> list[DeviceInfo]:
        """Return list of known devices, sorted by address."""
        with self._lock:
            return sorted(self.devices.values(), key=lambda d: d.addr)

    def get_device_by_index(self, idx: int) -> Optional[DeviceInfo]:
        devices = self.get_devices()
        if 0 <= idx < len(devices):
            return devices[idx]
        return None


def _find_ota_images() -> list[tuple[str, str]]:
    """Find available signed firmware images. Returns [(label, path), ...]."""
    firmware_dir = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "firmware"
    )
    pattern = os.path.join(firmware_dir, "build", "*",
                           "firmware", "zephyr", "zephyr.signed.bin")
    results = []
    for path in sorted(glob.glob(pattern)):
        # Extract board name from build/<board>/firmware/...
        board = path.split(os.sep)[-4]
        results.append((board, path))
    return results


def _parse_slot_hash(image_list_output: str, slot: int) -> Optional[str]:
    """Parse the hash of the image in the given slot from mcumgr image list output."""
    in_slot = False
    for line in image_list_output.splitlines():
        line = line.strip()
        if f"slot={slot}" in line:
            in_slot = True
        elif in_slot and line.startswith("hash:"):
            return line.split(":", 1)[1].strip()
        elif in_slot and "slot=" in line:
            break  # moved past target slot
    return None


# ── UCI command execution ────────────────────────────────────────────

def execute_command(cmd_name: str, device: DeviceInfo,
                    input_value: str = "") -> str:
    """Execute a UCI command against a device. Returns result string."""
    try:
        dev = UWBDeviceCoAP(device.ip, timeout=5.0)

        if cmd_name == "Get Info":
            result = dev.get_info()
            if "error" in result:
                return f"Error: {result['error']}"
            lines = [f"  {k}: {v}" for k, v in result.items()]
            return "\n".join(lines)

        elif cmd_name == "Get Status":
            result = dev.get_status()
            if "error" in result:
                return f"Error: {result['error']}"
            lines = [f"  {k}: {v}" for k, v in result.items()]
            return "\n".join(lines)

        elif cmd_name == "Start Ranging":
            return dev.start()

        elif cmd_name == "Stop Ranging":
            return dev.stop()

        elif cmd_name == "Set Interval":
            ms = int(input_value)
            return dev.set_interval(ms)

        elif cmd_name == "Set Address":
            addr = int(input_value, 0)
            return dev.set_addr(addr)

        elif cmd_name == "Set Role":
            return dev.set_role(input_value)

        elif cmd_name == "Calibrate":
            result = dev.calibrate()
            if "error" in result:
                return f"Error: {result['error']}\nEnsure ranging is active."
            offset = result["offset_mm"]
            return f"Calibration complete\nOffset: {offset} mm\nUse 'Save Config' to persist"

        elif cmd_name == "Get Cal Offset":
            result = dev.get_cal_offset()
            if "error" in result:
                return f"Error: {result['error']}"
            return f"Calibration offset: {result['offset_mm']} mm"

        elif cmd_name == "Set Cal Offset":
            offset = int(input_value)
            return dev.set_cal_offset(offset)

        elif cmd_name == "CIR Enable":
            count = int(input_value) if input_value.strip() else 0
            return dev.cir_enable(count)

        elif cmd_name == "CIR Disable":
            return dev.cir_disable()

        elif cmd_name == "Set Discovery Interval":
            interval = int(input_value)
            return dev.set_discovery_interval(interval)

        elif cmd_name == "Trigger Discovery":
            return dev.trigger_discovery()

        elif cmd_name == "Save Config":
            return dev.save()

        elif cmd_name == "Reboot":
            return dev.reboot()

        elif cmd_name == "Factory Reset":
            return dev.factory_reset()

        elif cmd_name == "Enter Bootloader":
            return dev.enter_bootloader()

        elif cmd_name == "Build All Firmware":
            firmware_dir = os.path.join(
                os.path.dirname(os.path.abspath(__file__)), "..", "firmware"
            )
            build_sh = os.path.join(firmware_dir, "build.sh")
            boards = ["nrf52840dk/nrf52840", "decawave_dwm3001cdk", "xiao_ble"]
            results = []
            for board in boards:
                r = subprocess.run(
                    ["bash", build_sh, board],
                    capture_output=True, text=True, timeout=300,
                )
                tag = "OK" if r.returncode == 0 else "FAIL"
                results.append(f"{board}: {tag}")
                if r.returncode != 0:
                    err = (r.stderr or r.stdout or "").strip()
                    results.append(err[-200:])
            return "\n".join(results)

        elif cmd_name == "OTA Upload Firmware":
            # input_value is the image path (set by pick list)
            image = input_value
            if not image or not os.path.isfile(image):
                return f"Error: Signed image not found: {image}"
            connstring = f"[{device.ip}]:1337"
            r = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "upload", image],
                capture_output=True, text=True, timeout=300,
            )
            if r.returncode != 0:
                return f"Upload failed:\n{(r.stderr or r.stdout).strip()}"
            # Verify slot 1 is populated
            r2 = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "list"],
                capture_output=True, text=True, timeout=15,
            )
            verify = (r2.stdout or "").strip()
            return (_OTA_PENDING +
                    f"Upload complete: {os.path.basename(image)}\n\n"
                    f"{verify}\n\n"
                    f"Use 'OTA Test Update' to test the new image.")

        elif cmd_name == "OTA Test Update":
            connstring = f"[{device.ip}]:1337"
            # Get slot 1 hash
            r = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "list"],
                capture_output=True, text=True, timeout=15,
            )
            if r.returncode != 0:
                return f"Error listing images:\n{(r.stderr or r.stdout).strip()}"
            # Parse hash from slot 1
            slot1_hash = _parse_slot_hash(r.stdout, 1)
            if not slot1_hash:
                return "Error: No image found in slot 1.\nUpload firmware first."
            # Mark for test
            r2 = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "test", slot1_hash],
                capture_output=True, text=True, timeout=15,
            )
            if r2.returncode != 0:
                return f"Test mark failed:\n{(r2.stderr or r2.stdout).strip()}"
            # Reset to boot into test image
            subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "reset"],
                capture_output=True, text=True, timeout=10,
            )
            return (_OTA_CONFIRMING +
                    f"Test update initiated.\n"
                    f"Device is rebooting into slot 1 image.\n\n"
                    f"After reboot, use 'OTA Confirm Update'\n"
                    f"to make permanent (or reboot again to rollback).")

        elif cmd_name == "OTA Confirm Update":
            connstring = f"[{device.ip}]:1337"
            # Get the running image hash (slot 0) — confirm without hash fails
            r = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "list"],
                capture_output=True, text=True, timeout=15,
            )
            if r.returncode != 0:
                return f"Error listing images:\n{(r.stderr or r.stdout).strip()}"
            slot0_hash = _parse_slot_hash(r.stdout, 0)
            if not slot0_hash:
                return "Error: Could not find running image hash."
            r2 = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "confirm", slot0_hash],
                capture_output=True, text=True, timeout=15,
            )
            if r2.returncode != 0:
                return f"Confirm failed:\n{(r2.stderr or r2.stdout).strip()}"
            return (_OTA_DONE +
                    f"Update confirmed.\n"
                    f"Running image is now permanent.")

        elif cmd_name == "OTA Image List":
            connstring = f"[{device.ip}]:1337"
            r = subprocess.run(
                ["mcumgr", "--conntype", "udp",
                 f"--connstring={connstring}", "image", "list"],
                capture_output=True, text=True, timeout=15,
            )
            if r.returncode != 0:
                return f"Error:\n{(r.stderr or r.stdout).strip()}"
            return (r.stdout or "").strip()

        else:
            return f"Unknown command: {cmd_name}"

    except Exception as e:
        return f"Error: {e}"


# ── Curses TUI ───────────────────────────────────────────────────────

class Dashboard:
    def __init__(self, stdscr, monitor: Optional[MonitorProcess],
                 tracker: DeviceTracker):
        self.stdscr = stdscr
        self.monitor = monitor
        self.tracker = tracker

        # UI state
        self.state = STATE_NORMAL
        self.cmd_index = 0
        self.dev_index = 0
        self.selected_cmd: str = ""
        self.input_buf: str = ""
        self.input_prompt: str = ""
        self.result_text: str = ""
        self.pick_index: int = 0
        self.pick_options: list[str] = []

        # Command execution thread
        self._exec_thread: Optional[threading.Thread] = None

        # Confirmation state
        self.confirm_action: str = ""

        # OTA status per device (keyed by IP)
        self.ota_status: dict[str, str] = {}
        self._ota_image_map: dict[str, str] = {}  # label → path

    def run(self):
        """Main loop — runs inside curses.wrapper."""
        self.stdscr.nodelay(True)
        self.stdscr.timeout(100)
        curses.curs_set(0)

        # Set up colors
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_GREEN, -1)   # online/running
        curses.init_pair(2, curses.COLOR_YELLOW, -1)   # stale/warning
        curses.init_pair(3, curses.COLOR_RED, -1)      # error/stopped
        curses.init_pair(4, curses.COLOR_CYAN, -1)     # header/highlight
        curses.init_pair(5, curses.COLOR_WHITE, curses.COLOR_BLUE)  # selected
        curses.init_pair(6, curses.COLOR_MAGENTA, -1)  # discovery events

        while True:
            try:
                key = self.stdscr.getch()
            except curses.error:
                key = -1

            if self._handle_input(key):
                break

            self._draw()

    def _handle_input(self, key: int) -> bool:
        """Handle key input. Returns True to quit."""
        if key == -1:
            return False

        if self.state == STATE_NORMAL:
            return self._handle_normal(key)
        elif self.state == STATE_SELECT_DEVICE:
            return self._handle_select_device(key)
        elif self.state == STATE_INPUT:
            return self._handle_input_mode(key)
        elif self.state == STATE_EXECUTING:
            return False  # no input during execution
        elif self.state == STATE_RESULT:
            return self._handle_result(key)
        elif self.state == STATE_CONFIRM:
            return self._handle_confirm(key)

        return False

    def _handle_normal(self, key: int) -> bool:
        if key == ord("q") or key == ord("Q"):
            return True
        elif key == curses.KEY_UP or key == ord("k"):
            self.cmd_index = max(0, self.cmd_index - 1)
        elif key == curses.KEY_DOWN or key == ord("j"):
            self.cmd_index = min(len(COMMANDS) - 1, self.cmd_index + 1)
        elif key in (curses.KEY_ENTER, ord("\n"), ord("\r")):
            self._select_command()
        return False

    def _select_command(self):
        """Handle Enter on a command."""
        self.selected_cmd = COMMANDS[self.cmd_index]

        if self.selected_cmd == "Toggle Monitor":
            if self.monitor:
                self.monitor.restart(not self.monitor.distance_only)
            return

        if self.selected_cmd == "Build All Firmware":
            self.confirm_action = self.selected_cmd
            self.state = STATE_CONFIRM
            return

        if self.selected_cmd in ("Reboot", "Factory Reset", "Enter Bootloader"):
            self.confirm_action = self.selected_cmd
            self.state = STATE_CONFIRM
            self.dev_index = 0
            return

        # OTA Upload needs confirm, then device select, then image pick
        if self.selected_cmd == "OTA Upload Firmware":
            self.confirm_action = self.selected_cmd
            self.state = STATE_CONFIRM
            self.dev_index = 0
            return

        # All other commands need a device
        devices = self.tracker.get_devices()
        if not devices:
            self.result_text = "No devices discovered yet.\nWait for scan or check Thread network."
            self.state = STATE_RESULT
            return

        if len(devices) == 1:
            # Auto-select single device
            self.dev_index = 0
            self._device_selected()
        else:
            self.dev_index = 0
            self.state = STATE_SELECT_DEVICE

    def _handle_select_device(self, key: int) -> bool:
        devices = self.tracker.get_devices()
        if key == 27:  # Escape
            self.state = STATE_NORMAL
        elif key == curses.KEY_UP or key == ord("k"):
            self.dev_index = max(0, self.dev_index - 1)
        elif key == curses.KEY_DOWN or key == ord("j"):
            self.dev_index = min(len(devices) - 1, self.dev_index + 1)
        elif key in (curses.KEY_ENTER, ord("\n"), ord("\r")):
            self._device_selected()
        return False

    def _device_selected(self):
        """A device was selected — proceed to input or execute."""
        if self.selected_cmd in INPUT_COMMANDS:
            self.input_buf = ""
            if self.selected_cmd == "Set Interval":
                self.input_prompt = "Interval (ms): "
            elif self.selected_cmd == "Set Address":
                self.input_prompt = "Address (0xNNNN): "
            elif self.selected_cmd == "Set Cal Offset":
                self.input_prompt = "Offset (mm): "
            elif self.selected_cmd == "CIR Enable":
                self.input_prompt = "Cycle count (empty=continuous): "
            elif self.selected_cmd == "Set Discovery Interval":
                self.input_prompt = "Interval (cycles, 0=off): "
            self.state = STATE_INPUT
            curses.curs_set(1)
        elif self.selected_cmd in PICK_COMMANDS:
            if self.selected_cmd == "OTA Upload Firmware":
                images = _find_ota_images()
                if not images:
                    self.result_text = ("No signed images found.\n"
                                        "Build first with 'Build All Firmware'.")
                    self.state = STATE_RESULT
                    return
                self._ota_image_map = {label: path for label, path in images}
                self.pick_options = [label for label, _ in images]
            else:
                self.pick_options = ROLE_OPTIONS
            self.pick_index = 0
            self.state = STATE_INPUT  # reuse input state for pick list
            self.input_prompt = ""
        else:
            self._execute()

    def _handle_input_mode(self, key: int) -> bool:
        if self.selected_cmd in PICK_COMMANDS and not self.input_prompt:
            # Pick list mode
            if key == 27:  # Escape
                self.state = STATE_NORMAL
                curses.curs_set(0)
            elif key == curses.KEY_UP or key == ord("k"):
                self.pick_index = max(0, self.pick_index - 1)
            elif key == curses.KEY_DOWN or key == ord("j"):
                self.pick_index = min(
                    len(self.pick_options) - 1, self.pick_index + 1
                )
            elif key in (curses.KEY_ENTER, ord("\n"), ord("\r")):
                selected = self.pick_options[self.pick_index]
                if self.selected_cmd == "OTA Upload Firmware":
                    # Translate board label to image path
                    self.input_buf = self._ota_image_map.get(selected, "")
                else:
                    self.input_buf = selected
                curses.curs_set(0)
                self._execute()
        else:
            # Text input mode
            if key == 27:  # Escape
                self.state = STATE_NORMAL
                curses.curs_set(0)
            elif key in (curses.KEY_ENTER, ord("\n"), ord("\r")):
                if self.input_buf or self.selected_cmd == "CIR Enable":
                    curses.curs_set(0)
                    self._execute()
            elif key in (curses.KEY_BACKSPACE, 127, 8):
                self.input_buf = self.input_buf[:-1]
            elif 32 <= key <= 126:
                self.input_buf += chr(key)

        return False

    def _handle_result(self, key: int) -> bool:
        # Any key dismisses the result
        if key != -1:
            self.state = STATE_NORMAL
        return False

    def _handle_confirm(self, key: int) -> bool:
        if key == ord("y") or key == ord("Y"):
            if self.confirm_action == "Build All Firmware":
                # No device needed — execute directly
                self._execute_no_device()
                return False
            # Confirmed — select device
            devices = self.tracker.get_devices()
            if not devices:
                self.result_text = "No devices discovered."
                self.state = STATE_RESULT
                return False
            if len(devices) == 1:
                self.dev_index = 0
                self._device_selected()
            else:
                self.dev_index = 0
                self.state = STATE_SELECT_DEVICE
        elif key == ord("n") or key == ord("N") or key == 27:
            self.state = STATE_NORMAL
        return False

    def _execute_no_device(self):
        """Run a command that doesn't target a specific device."""
        self.state = STATE_EXECUTING
        cmd = self.selected_cmd

        def _run():
            self.result_text = execute_command(cmd, DeviceInfo(ip=""), "")
            self.state = STATE_RESULT

        self._exec_thread = threading.Thread(target=_run, daemon=True)
        self._exec_thread.start()

    def _execute(self):
        """Run the selected command in a background thread."""
        device = self.tracker.get_device_by_index(self.dev_index)
        if not device:
            self.result_text = "Device not found."
            self.state = STATE_RESULT
            return

        self.state = STATE_EXECUTING
        cmd = self.selected_cmd
        val = self.input_buf
        ip = device.ip

        def _run():
            result = execute_command(cmd, device, val)
            # Check for OTA status markers and update tracking
            if result.startswith(_OTA_PENDING):
                self.ota_status[ip] = "update pending"
                result = result[len(_OTA_PENDING):]
            elif result.startswith(_OTA_CONFIRMING):
                self.ota_status[ip] = "confirmation pending"
                result = result[len(_OTA_CONFIRMING):]
            elif result.startswith(_OTA_DONE):
                self.ota_status.pop(ip, None)
                result = result[len(_OTA_DONE):]
            self.result_text = result
            self.state = STATE_RESULT

        self._exec_thread = threading.Thread(target=_run, daemon=True)
        self._exec_thread.start()

    # ── Drawing ──────────────────────────────────────────────────────

    def _draw(self):
        """Redraw the entire screen."""
        self.stdscr.erase()
        height, width = self.stdscr.getmaxyx()

        if height < 10 or width < 40:
            self._draw_too_small(height, width)
            self.stdscr.noutrefresh()
            curses.doupdate()
            return

        # Layout: top half = monitor, bottom half = commands + devices
        mid_y = height // 2
        bot_left_w = width // 2
        bot_right_w = width - bot_left_w

        self._draw_monitor_panel(0, 0, mid_y, width)
        self._draw_command_panel(mid_y, 0, height - mid_y, bot_left_w)
        self._draw_device_panel(mid_y, bot_left_w, height - mid_y, bot_right_w)

        # Overlay for modal states
        if self.state == STATE_SELECT_DEVICE:
            self._draw_device_select_overlay(height, width)
        elif self.state == STATE_INPUT:
            self._draw_input_overlay(height, width)
        elif self.state == STATE_EXECUTING:
            self._draw_executing_overlay(height, width)
        elif self.state == STATE_RESULT:
            self._draw_result_overlay(height, width)
        elif self.state == STATE_CONFIRM:
            self._draw_confirm_overlay(height, width)

        self.stdscr.noutrefresh()
        curses.doupdate()

    def _draw_too_small(self, h, w):
        msg = "Terminal too small"
        try:
            self.stdscr.addstr(0, 0, msg[:w - 1])
        except curses.error:
            pass

    def _safe_addstr(self, y, x, text, attr=0):
        """Write text to screen, truncating to fit."""
        h, w = self.stdscr.getmaxyx()
        if y < 0 or y >= h or x >= w:
            return
        max_len = w - x - 1
        if max_len <= 0:
            return
        try:
            self.stdscr.addnstr(y, x, text, max_len, attr)
        except curses.error:
            pass

    def _draw_hline(self, y, x, width, char=None):
        if char is None:
            char = curses.ACS_HLINE
        h, w = self.stdscr.getmaxyx()
        if y < 0 or y >= h:
            return
        draw_w = min(width, w - x - 1)
        if draw_w <= 0:
            return
        try:
            self.stdscr.hline(y, x, char, draw_w)
        except curses.error:
            pass

    def _draw_vline(self, y, x, height, char=None):
        if char is None:
            char = curses.ACS_VLINE
        h, w = self.stdscr.getmaxyx()
        if x < 0 or x >= w:
            return
        draw_h = min(height, h - y)
        if draw_h <= 0:
            return
        try:
            self.stdscr.vline(y, x, char, draw_h)
        except curses.error:
            pass

    def _draw_monitor_panel(self, y, x, height, width):
        """Draw the top monitor output panel."""
        mode = self.monitor.mode_label if self.monitor else "disabled"
        header = f" Monitor: {mode} "
        self._safe_addstr(y, x, header, curses.color_pair(4) | curses.A_BOLD)
        self._draw_hline(y + 1, x, width)

        if not self.monitor:
            self._safe_addstr(
                y + 2, x + 1,
                "Monitor disabled (--no-monitor). "
                "Run monitor.py separately.",
                curses.color_pair(2),
            )
            return

        lines = self.monitor.get_lines()
        avail = height - 2  # header + separator
        visible = lines[-avail:] if len(lines) > avail else lines

        for i, line in enumerate(visible):
            row = y + 2 + i
            attr = 0
            if "[DIST" in line:
                if "[NLOS!]" in line:
                    attr = curses.color_pair(3)  # RED — high confidence NLOS
                elif "[NLOS?]" in line:
                    attr = curses.color_pair(2)  # YELLOW — likely NLOS
                else:
                    attr = curses.color_pair(1)  # GREEN — LOS
            elif "[EVT" in line:
                if "DISC_RX" in line:
                    attr = curses.color_pair(6) | curses.A_BOLD  # magenta
                else:
                    attr = curses.color_pair(4)
            elif "[WARN" in line or "[dashboard]" in line:
                attr = curses.color_pair(2)
            self._safe_addstr(row, x + 1, line, attr)

    def _draw_command_panel(self, y, x, height, width):
        """Draw the bottom-left command panel."""
        header = " Commands "
        self._safe_addstr(y, x, header, curses.color_pair(4) | curses.A_BOLD)
        self._draw_hline(y, x + len(header), width - len(header))

        avail = height - 2  # header + footer

        for i, cmd in enumerate(COMMANDS):
            row = y + 1 + i
            if row >= y + height - 1:
                break

            if i == self.cmd_index and self.state == STATE_NORMAL:
                marker = "> "
                attr = curses.color_pair(5) | curses.A_BOLD
            else:
                marker = "  "
                attr = 0

            self._safe_addstr(row, x, f"{marker}{cmd}", attr)

        # Footer
        footer_y = y + height - 1
        if self.state == STATE_NORMAL:
            footer = " [↑↓/jk] nav  [Enter] select  [q] quit"
        else:
            footer = " [Esc] cancel"
        self._safe_addstr(footer_y, x, footer, curses.A_DIM)

    def _draw_device_panel(self, y, x, height, width):
        """Draw the bottom-right device panel."""
        header = " Devices "
        self._safe_addstr(y, x, header, curses.color_pair(4) | curses.A_BOLD)
        self._draw_hline(y, x + len(header), width - len(header))
        self._draw_vline(y, x, height)

        devices = self.tracker.get_devices()
        now = time.monotonic()
        row = y + 1

        if not devices:
            self._safe_addstr(row, x + 2, "Scanning...", curses.color_pair(2))
            row += 1
        else:
            for dev in devices:
                if row >= y + height - 2:
                    break

                stale = (now - dev.last_seen) > 30 if dev.last_seen else True
                dot_color = curses.color_pair(2) if stale else curses.color_pair(1)

                # Line 1: dot + addr + role + version
                line1 = f" {dev.addr} {dev.role:<7s} v{dev.fw_version}"
                self._safe_addstr(row, x + 1, "●", dot_color)
                self._safe_addstr(row, x + 3, line1.rstrip())
                row += 1

                if row >= y + height - 2:
                    break

                # Line 2: IP (truncated)
                ip_display = dev.ip
                max_ip = width - 4
                if len(ip_display) > max_ip:
                    ip_display = ip_display[:max_ip - 1] + "…"
                self._safe_addstr(row, x + 4, ip_display, curses.A_DIM)
                row += 1

                if row >= y + height - 2:
                    break

                # Line 3: status summary
                if dev.running:
                    if dev.role == "ANCHOR":
                        status = f"running  dist={dev.last_distance_m:.2f}m"
                    else:
                        status = f"running  cnt={dev.range_count}"
                    self._safe_addstr(
                        row, x + 4, status, curses.color_pair(1)
                    )
                else:
                    self._safe_addstr(
                        row, x + 4, "stopped", curses.color_pair(3)
                    )
                row += 1

                # Line 4: OTA status (if any)
                ota = self.ota_status.get(dev.ip, "")
                if ota and row < y + height - 2:
                    if ota == "update pending":
                        attr = curses.color_pair(2)  # yellow
                    else:
                        attr = curses.color_pair(4)  # cyan
                    self._safe_addstr(row, x + 4, ota, attr | curses.A_BOLD)
                    row += 1

                # Blank line between devices
                row += 1

        # Footer: last scan time
        footer_y = y + height - 1
        if self.tracker.last_scan > 0:
            ago = int(now - self.tracker.last_scan)
            scan_text = f" Last scan: {ago}s ago"
        else:
            scan_text = " Scanning..."
        if self.tracker.scan_error:
            scan_text += f"  ERR: {self.tracker.scan_error}"
        self._safe_addstr(footer_y, x + 1, scan_text, curses.A_DIM)

    # ── Modal overlays ───────────────────────────────────────────────

    def _draw_box(self, y, x, h, w, title=""):
        """Draw a bordered box."""
        try:
            # Top border
            self.stdscr.addch(y, x, curses.ACS_ULCORNER)
            self.stdscr.hline(y, x + 1, curses.ACS_HLINE, w - 2)
            self.stdscr.addch(y, x + w - 1, curses.ACS_URCORNER)
            # Sides
            for row in range(y + 1, y + h - 1):
                self.stdscr.addch(row, x, curses.ACS_VLINE)
                # Clear interior
                self.stdscr.addnstr(row, x + 1, " " * (w - 2), w - 2)
                self.stdscr.addch(row, x + w - 1, curses.ACS_VLINE)
            # Bottom border
            self.stdscr.addch(y + h - 1, x, curses.ACS_LLCORNER)
            self.stdscr.hline(y + h - 1, x + 1, curses.ACS_HLINE, w - 2)
            self.stdscr.addch(y + h - 1, x + w - 1, curses.ACS_LRCORNER)
        except curses.error:
            pass

        if title:
            self._safe_addstr(y, x + 2, f" {title} ",
                              curses.color_pair(4) | curses.A_BOLD)

    def _draw_device_select_overlay(self, scr_h, scr_w):
        """Overlay: pick a device."""
        devices = self.tracker.get_devices()
        box_h = min(len(devices) + 4, scr_h - 4)
        box_w = min(50, scr_w - 4)
        box_y = (scr_h - box_h) // 2
        box_x = (scr_w - box_w) // 2

        self._draw_box(box_y, box_x, box_h, box_w, "Select Device")

        for i, dev in enumerate(devices):
            row = box_y + 1 + i
            if row >= box_y + box_h - 2:
                break

            if i == self.dev_index:
                attr = curses.color_pair(5) | curses.A_BOLD
                marker = "> "
            else:
                attr = 0
                marker = "  "

            label = f"{marker}{dev.addr} {dev.role:<7s} {dev.ip}"
            self._safe_addstr(row, box_x + 1, label[:box_w - 3], attr)

        footer_y = box_y + box_h - 2
        self._safe_addstr(
            footer_y, box_x + 2,
            "[↑↓] navigate  [Enter] select  [Esc] cancel",
            curses.A_DIM,
        )

    def _draw_input_overlay(self, scr_h, scr_w):
        """Overlay: text input or pick list."""
        if self.selected_cmd in PICK_COMMANDS and not self.input_prompt:
            # Pick list
            box_h = len(self.pick_options) + 4
            box_w = min(40, scr_w - 4)
            box_y = (scr_h - box_h) // 2
            box_x = (scr_w - box_w) // 2

            self._draw_box(box_y, box_x, box_h, box_w, self.selected_cmd)

            for i, opt in enumerate(self.pick_options):
                row = box_y + 1 + i
                if i == self.pick_index:
                    attr = curses.color_pair(5) | curses.A_BOLD
                    marker = "> "
                else:
                    attr = 0
                    marker = "  "
                self._safe_addstr(row, box_x + 1, f"{marker}{opt}", attr)

            footer_y = box_y + box_h - 2
            self._safe_addstr(
                footer_y, box_x + 2,
                "[↑↓] navigate  [Enter] select  [Esc] cancel",
                curses.A_DIM,
            )
        else:
            # Text input
            box_h = 5
            box_w = min(50, scr_w - 4)
            box_y = (scr_h - box_h) // 2
            box_x = (scr_w - box_w) // 2

            self._draw_box(box_y, box_x, box_h, box_w, self.selected_cmd)

            prompt_text = self.input_prompt + self.input_buf
            self._safe_addstr(box_y + 1, box_x + 2, prompt_text)

            # Position cursor
            cursor_x = box_x + 2 + len(self.input_prompt) + len(self.input_buf)
            if cursor_x < box_x + box_w - 1:
                try:
                    self.stdscr.move(box_y + 1, cursor_x)
                except curses.error:
                    pass

            self._safe_addstr(
                box_y + 3, box_x + 2,
                "[Enter] confirm  [Esc] cancel",
                curses.A_DIM,
            )

    def _draw_executing_overlay(self, scr_h, scr_w):
        """Overlay: command in progress."""
        box_h = 3
        box_w = min(30, scr_w - 4)
        box_y = (scr_h - box_h) // 2
        box_x = (scr_w - box_w) // 2

        self._draw_box(box_y, box_x, box_h, box_w)
        self._safe_addstr(
            box_y + 1, box_x + 2,
            "Sending...",
            curses.color_pair(2) | curses.A_BOLD,
        )

    def _draw_result_overlay(self, scr_h, scr_w):
        """Overlay: show command result."""
        lines = self.result_text.split("\n")
        max_line = max((len(l) for l in lines), default=10)
        box_w = min(max(max_line + 6, 30), scr_w - 4)
        box_h = min(len(lines) + 4, scr_h - 4)
        box_y = (scr_h - box_h) // 2
        box_x = (scr_w - box_w) // 2

        self._draw_box(box_y, box_x, box_h, box_w, "Result")

        for i, line in enumerate(lines):
            row = box_y + 1 + i
            if row >= box_y + box_h - 2:
                break
            attr = 0
            if "Error" in line or "ERR" in line:
                attr = curses.color_pair(3)
            elif "OK" in line:
                attr = curses.color_pair(1)
            self._safe_addstr(row, box_x + 2, line, attr)

        footer_y = box_y + box_h - 2
        self._safe_addstr(
            footer_y, box_x + 2, "Press any key to dismiss", curses.A_DIM
        )

    def _draw_confirm_overlay(self, scr_h, scr_w):
        """Overlay: confirmation prompt."""
        box_h = 5
        box_w = min(45, scr_w - 4)
        box_y = (scr_h - box_h) // 2
        box_x = (scr_w - box_w) // 2

        self._draw_box(box_y, box_x, box_h, box_w, "Confirm")
        messages = {
            "Factory Reset": " This erases all config.",
            "Enter Bootloader": " Device will enter bootloader.",
            "Build All Firmware": " Builds all 3 board targets.",
            "OTA Upload Firmware": " Upload firmware over Thread.",
        }
        msg = messages.get(self.confirm_action, " Device will restart.")
        self._safe_addstr(
            box_y + 1, box_x + 2,
            f"{self.confirm_action}?{msg}",
            curses.color_pair(3) | curses.A_BOLD,
        )
        self._safe_addstr(
            box_y + 3, box_x + 2,
            "[y] confirm  [n/Esc] cancel",
            curses.A_DIM,
        )


# ── Entry point ──────────────────────────────────────────────────────

def main(stdscr, args):
    monitor = None
    if not args.no_monitor:
        tools_dir = os.path.dirname(os.path.abspath(__file__))
        monitor = MonitorProcess(tools_dir)
        monitor.start()

    tracker = DeviceTracker(iface=args.iface)
    tracker.start()

    try:
        dashboard = Dashboard(stdscr, monitor, tracker)
        dashboard.run()
    finally:
        tracker.stop()
        if monitor:
            monitor.stop()


def cli():
    parser = argparse.ArgumentParser(
        description="UWB Dashboard — curses TUI for monitor + device control"
    )
    parser.add_argument(
        "--no-monitor", action="store_true",
        help="skip starting monitor.py subprocess",
    )
    parser.add_argument(
        "--iface", default="wpan0",
        help="Thread network interface (default: wpan0)",
    )
    args = parser.parse_args()

    curses.wrapper(lambda stdscr: main(stdscr, args))


if __name__ == "__main__":
    cli()
