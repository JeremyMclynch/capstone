#!/usr/bin/env python3
"""
UWB Device Configuration Tool — UCI binary protocol over serial or CoAP.

Usage:
    python3 uwb_tool.py <target> <command> [args...]

Target:
    /dev/ttyACM1              Serial UART transport
    coap://[ipv6-addr]        CoAP transport (port 5683)
    coap://[ipv6-addr]:port   CoAP transport (explicit port)

Commands:
    info                        Get device info (version, role, address, state)
    status                      Get ranging status (running, distance, count)
    set-role <anchor|tag>       Set node role (requires save + reboot)
    set-addr <0xNNNN>           Set UWB short address
    set-interval <ms>           Set ranging interval in milliseconds
    set-server <ipv6> <port>    Set CoAP server address and port
    start                       Start ranging
    stop                        Stop ranging
    calibrate                   Auto-calibrate: compute offset from 1m reference
    get-cal-offset              Get current calibration offset (mm)
    set-cal-offset <mm>         Manually set calibration offset (mm)
    save                        Save config to flash (persists across reboots)
    factory-reset               Erase config and reboot with defaults
    reboot                      Reboot the device
    enter-bootloader            Reboot into MCUboot serial recovery mode

Examples:
    python3 uwb_tool.py /dev/ttyACM1 info
    python3 uwb_tool.py /dev/ttyACM3 set-role tag
    python3 uwb_tool.py coap://[fdde:ad00:beef:0:a]  info
    python3 uwb_tool.py coap://[fdde:ad00:beef:0:a]  set-interval 500
"""

import sys
import struct
import time
import re

# ── Protocol constants ───────────────────────────────────────────────

SYNC_REQ = 0xAA
SYNC_RSP = 0xBB

CMD_GET_INFO      = 0x01
CMD_SET_ROLE      = 0x02
CMD_SET_ADDR      = 0x03
CMD_SET_INTERVAL  = 0x04
CMD_SET_SERVER    = 0x05
CMD_START         = 0x10
CMD_STOP          = 0x11
CMD_GET_STATUS    = 0x12
CMD_SAVE_CONFIG   = 0x20
CMD_FACTORY_RESET = 0x21
CMD_ENTER_BOOTLOADER = 0x22
CMD_REBOOT           = 0x23
CMD_CALIBRATE        = 0x30
CMD_SET_CAL_OFFSET   = 0x31
CMD_GET_CAL_OFFSET   = 0x32

STATUS_NAMES = {
    0x00: "OK",
    0x01: "ERR_UNKNOWN_CMD",
    0x02: "ERR_BAD_PAYLOAD",
    0x03: "ERR_INVALID_VALUE",
    0x04: "ERR_BUSY",
}

ROLE_ANCHOR = 0
ROLE_TAG    = 1

# ── CRC-8 (polynomial 0x07, init 0x00) ──────────────────────────────

def crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
            crc &= 0xFF
    return crc


# ── Base device class (transport-agnostic) ───────────────────────────

class UWBDeviceBase:
    """UCI device — subclass must implement _send_request() and close()."""

    def _send_request(self, cmd: int, payload: bytes = b"") -> tuple:
        raise NotImplementedError

    def close(self):
        pass

    def get_info(self) -> dict:
        status, payload = self._send_request(CMD_GET_INFO)
        if status != 0 or len(payload) < 6:
            return {"error": STATUS_NAMES.get(status, f"0x{status:02X}")}
        return {
            "fw_version": f"{payload[0]}.{payload[1]}",
            "role": "TAG" if payload[2] == ROLE_TAG else "ANCHOR",
            "addr": f"0x{payload[3] | (payload[4] << 8):04X}",
            "running": bool(payload[5]),
        }

    def get_status(self) -> dict:
        status, payload = self._send_request(CMD_GET_STATUS)
        if status != 0 or len(payload) < 13:
            return {"error": STATUS_NAMES.get(status, f"0x{status:02X}")}
        running = bool(payload[0])
        dist_mm = struct.unpack_from("<I", payload, 1)[0]
        uptime  = struct.unpack_from("<I", payload, 5)[0]
        count   = struct.unpack_from("<I", payload, 9)[0]
        return {
            "running": running,
            "last_distance_m": dist_mm / 1000.0,
            "uptime_s": uptime,
            "range_count": count,
        }

    def set_role(self, role: str) -> str:
        role_byte = ROLE_TAG if role.lower() == "tag" else ROLE_ANCHOR
        status, _ = self._send_request(CMD_SET_ROLE, bytes([role_byte]))
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def set_addr(self, addr: int) -> str:
        payload = struct.pack("<H", addr)
        status, _ = self._send_request(CMD_SET_ADDR, payload)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def set_interval(self, ms: int) -> str:
        payload = struct.pack("<H", ms)
        status, _ = self._send_request(CMD_SET_INTERVAL, payload)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def set_server(self, addr: str, port: int) -> str:
        addr_bytes = addr.encode("ascii") + b"\x00"
        payload = addr_bytes + struct.pack("<H", port)
        status, _ = self._send_request(CMD_SET_SERVER, payload)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def start(self) -> str:
        status, _ = self._send_request(CMD_START)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def stop(self) -> str:
        status, _ = self._send_request(CMD_STOP)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def save(self) -> str:
        status, _ = self._send_request(CMD_SAVE_CONFIG)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def factory_reset(self) -> str:
        status, _ = self._send_request(CMD_FACTORY_RESET)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def enter_bootloader(self) -> str:
        status, _ = self._send_request(CMD_ENTER_BOOTLOADER)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def reboot(self) -> str:
        status, _ = self._send_request(CMD_REBOOT)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def calibrate(self) -> dict:
        status, payload = self._send_request(CMD_CALIBRATE)
        if status != 0 or len(payload) < 2:
            return {"error": STATUS_NAMES.get(status, f"0x{status:02X}")}
        offset = struct.unpack_from("<h", payload, 0)[0]
        return {"offset_mm": offset}

    def set_cal_offset(self, offset_mm: int) -> str:
        payload = struct.pack("<h", offset_mm)
        status, _ = self._send_request(CMD_SET_CAL_OFFSET, payload)
        return STATUS_NAMES.get(status, f"0x{status:02X}")

    def get_cal_offset(self) -> dict:
        status, payload = self._send_request(CMD_GET_CAL_OFFSET)
        if status != 0 or len(payload) < 2:
            return {"error": STATUS_NAMES.get(status, f"0x{status:02X}")}
        offset = struct.unpack_from("<h", payload, 0)[0]
        return {"offset_mm": offset}


# ── Serial UART transport ────────────────────────────────────────────

class UWBDevice(UWBDeviceBase):
    """UCI over serial UART."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 2.0):
        import serial as _serial
        self.ser = _serial.Serial(port, baudrate, timeout=timeout)
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def _send_request(self, cmd: int, payload: bytes = b"") -> tuple:
        """Send a UCI request over UART and return (status, payload)."""
        frame = bytes([SYNC_REQ, cmd, len(payload)]) + payload
        frame += bytes([crc8(frame)])
        self.ser.write(frame)

        # Read response: scan for SYNC_RSP byte (skip log output)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            b = self.ser.read(1)
            if not b:
                raise TimeoutError("No response from device")
            if b[0] == SYNC_RSP:
                break
        else:
            raise TimeoutError("No SYNC_RSP found in response")

        # Read cmd(1) + status(1) + len(1)
        header = self.ser.read(3)
        if len(header) < 3:
            raise TimeoutError("Incomplete response header")

        rsp_cmd, rsp_status, rsp_len = header[0], header[1], header[2]

        rsp_payload = b""
        if rsp_len > 0:
            rsp_payload = self.ser.read(rsp_len)
            if len(rsp_payload) < rsp_len:
                raise TimeoutError("Incomplete response payload")

        rsp_crc = self.ser.read(1)
        if len(rsp_crc) < 1:
            raise TimeoutError("Missing response CRC")

        # Verify CRC
        full_frame = bytes([SYNC_RSP, rsp_cmd, rsp_status, rsp_len]) + rsp_payload
        expected_crc = crc8(full_frame)
        if rsp_crc[0] != expected_crc:
            raise ValueError(
                f"CRC mismatch: got 0x{rsp_crc[0]:02X}, expected 0x{expected_crc:02X}"
            )

        return rsp_status, rsp_payload


# ── CoAP transport ───────────────────────────────────────────────────

class UWBDeviceCoAP(UWBDeviceBase):
    """UCI over CoAP/Thread (POST /cmd)."""

    def __init__(self, host: str, port: int = 5683, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.uri = f"coap://[{host}]:{port}/cmd"

    def _send_request(self, cmd: int, payload: bytes = b"") -> tuple:
        """Send a UCI request via CoAP POST and return (status, payload)."""
        import asyncio
        return asyncio.run(self._async_send(cmd, payload))

    async def _async_send(self, cmd: int, payload: bytes) -> tuple:
        import asyncio
        import aiocoap

        # Build UCI-over-CoAP payload: [CMD] [LEN] [PAYLOAD...]
        coap_payload = bytes([cmd, len(payload)]) + payload

        request = aiocoap.Message(
            code=aiocoap.POST,
            uri=self.uri,
            payload=coap_payload,
        )

        context = await aiocoap.Context.create_client_context()
        try:
            response = await asyncio.wait_for(
                context.request(request).response,
                timeout=self.timeout,
            )
        finally:
            await context.shutdown()

        # Parse UCI response: [CMD:1] [STATUS:1] [LEN:1] [PAYLOAD:0..N]
        rsp = response.payload
        if len(rsp) < 3:
            raise ValueError(f"Response too short: {len(rsp)} bytes")

        rsp_status = rsp[1]
        rsp_len = rsp[2]
        rsp_payload = rsp[3:3 + rsp_len] if rsp_len > 0 else b""

        if len(rsp_payload) < rsp_len:
            raise ValueError(
                f"Response payload truncated: expected {rsp_len}, got {len(rsp_payload)}"
            )

        return rsp_status, rsp_payload

    def factory_reset(self) -> str:
        try:
            return super().factory_reset()
        except Exception:
            # Device reboots before response — expected
            return "OK (device rebooting)"

    def enter_bootloader(self) -> str:
        try:
            return super().enter_bootloader()
        except Exception:
            return "OK (device entering bootloader)"

    def reboot(self) -> str:
        try:
            return super().reboot()
        except Exception:
            return "OK (device rebooting)"


# ── CLI entry point ─────────────────────────────────────────────────

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    target = sys.argv[1]
    cmd    = sys.argv[2].lower()
    args   = sys.argv[3:]

    # Detect transport: coap:// prefix → CoAP, otherwise serial
    if target.startswith("coap://"):
        m = re.match(r'coap://\[?([^\]]+)\]?(?::(\d+))?$', target)
        if not m:
            print(f"Invalid CoAP URI: {target}")
            sys.exit(1)
        host = m.group(1)
        port = int(m.group(2)) if m.group(2) else 5683
        dev = UWBDeviceCoAP(host, port)
    else:
        dev = UWBDevice(target)

    try:
        if cmd == "info":
            result = dev.get_info()
            for k, v in result.items():
                print(f"  {k}: {v}")

        elif cmd == "status":
            result = dev.get_status()
            for k, v in result.items():
                print(f"  {k}: {v}")

        elif cmd == "set-role":
            if not args or args[0].lower() not in ("anchor", "tag"):
                print("Usage: set-role <anchor|tag>")
                sys.exit(1)
            result = dev.set_role(args[0])
            print(f"  set-role: {result}")
            if result == "OK":
                print("  Note: save + reboot required to apply role change")

        elif cmd == "set-addr":
            if not args:
                print("Usage: set-addr <0xNNNN>")
                sys.exit(1)
            addr = int(args[0], 0)
            result = dev.set_addr(addr)
            print(f"  set-addr: {result}")

        elif cmd == "set-interval":
            if not args:
                print("Usage: set-interval <ms>")
                sys.exit(1)
            ms = int(args[0])
            result = dev.set_interval(ms)
            print(f"  set-interval: {result}")

        elif cmd == "set-server":
            if len(args) < 2:
                print("Usage: set-server <ipv6-addr> <port>")
                sys.exit(1)
            result = dev.set_server(args[0], int(args[1]))
            print(f"  set-server: {result}")

        elif cmd == "start":
            result = dev.start()
            print(f"  start: {result}")

        elif cmd == "stop":
            result = dev.stop()
            print(f"  stop: {result}")

        elif cmd == "save":
            result = dev.save()
            print(f"  save: {result}")

        elif cmd in ("factory-reset", "reset"):
            result = dev.factory_reset()
            print(f"  factory-reset: {result}")

        elif cmd in ("enter-bootloader", "bootloader", "recovery"):
            result = dev.enter_bootloader()
            print(f"  enter-bootloader: {result}")
            if "OK" in result:
                print("  Device is now in MCUboot serial recovery mode.")
                print("  Upload firmware:")
                print("    mcumgr --conntype serial --connstring dev=<port>,baud=115200 image upload <file>")

        elif cmd == "reboot":
            result = dev.reboot()
            print(f"  reboot: {result}")

        elif cmd == "calibrate":
            result = dev.calibrate()
            if "error" in result:
                print(f"  calibrate: Error — {result['error']}")
                print("  Ensure ranging is active with measurements before calibrating.")
            else:
                print(f"  calibrate: OK")
                print(f"  offset: {result['offset_mm']} mm")
                print("  Use 'save' to persist calibration to flash.")

        elif cmd == "set-cal-offset":
            if not args:
                print("Usage: set-cal-offset <mm>")
                sys.exit(1)
            offset = int(args[0])
            result = dev.set_cal_offset(offset)
            print(f"  set-cal-offset: {result}")

        elif cmd == "get-cal-offset":
            result = dev.get_cal_offset()
            if "error" in result:
                print(f"  get-cal-offset: Error — {result['error']}")
            else:
                print(f"  offset: {result['offset_mm']} mm")

        else:
            print(f"Unknown command: {cmd}")
            print(__doc__)
            sys.exit(1)

    finally:
        dev.close()


if __name__ == "__main__":
    main()
