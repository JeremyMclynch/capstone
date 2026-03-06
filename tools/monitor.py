#!/usr/bin/env python3
"""
UWB monitor — receives CoAP POSTs from anchor (distance) and tag (events),
and optionally reads distance measurements from a serial port.

Usage:
    python3 monitor.py                           # CoAP only
    python3 monitor.py -s /dev/ttyACM1           # CoAP + serial
    python3 monitor.py -d -s /dev/ttyACM1        # distance only, with serial

Resources:
  POST /distance  — from anchor, 20-byte distance measurement (12-byte legacy supported)
  POST /event     — from tag,    6-byte UWB packet event

Dependencies:
    pip install aiocoap pyserial
"""

import argparse
import asyncio
import re
import struct
import time
import aiocoap
import aiocoap.resource as resource

# /distance payload (20 bytes): anchor_id(u16), tag_id(u16), distance_mm(u32),
#   uptime_s(u32), rssi_q8(i16), fp_power_q8(i16), fp_index(u16), peak_index(u16)
DIST_FMT_V2 = struct.Struct("<HHIIhhHH")  # 20 bytes
DIST_FMT_V1 = struct.Struct("<HHII")       # 12 bytes (legacy)

# /event payload: node_id (u16), event (u8), seq (u8), reserved (u16)
EVT_FMT  = struct.Struct("<HBBxx")
EVT_SIZE = EVT_FMT.size   # 6 bytes

# /cir payload header: anchor_id(u16), tag_id(u16), distance_mm(u32), uptime_s(u32),
#   fp_index(u16), sample_offset(u16), num_samples(u8), read_mode(u8), reserved(u16)
CIR_HDR_FMT = struct.Struct("<HHIIHHBBxx")  # 20 bytes

EVT_NAMES = {
    0x01: "POLL_TX",
    0x02: "RESP_RX",
    0x03: "FINAL_TX",
    0x10: "NO_RESP",
    0x20: "DISC_RX",
}

_dist_count = 0
_evt_count  = 0
_cir_count  = 0
_t_last_dist: dict[tuple[int, int], float] = {}

# ANSI colors
_YELLOW = "\033[33m"
_RED    = "\033[31m"
_CYAN   = "\033[36m"
_RESET  = "\033[0m"


def _nlos_flag(rssi_dbm, fp_dbm, fp_idx, peak_index):
    """Return (level, flag) — 0=LOS, 1=likely NLOS, 2=high-confidence NLOS."""
    multipath = (rssi_dbm - fp_dbm) > 6.0
    path_diff = (peak_index - fp_idx) > 5.0
    if multipath and path_diff:
        return 2, f"  {_RED}[NLOS!]{_RESET}"
    elif multipath or path_diff:
        return 1, f"  {_YELLOW}[NLOS?]{_RESET}"
    return 0, ""

# Regex to match Zephyr log lines with ANSI escapes:
# [00:00:01.234,000] <inf> uwb_manager: Distance: -0.123 m (tag=0x0100)
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
_SERIAL_DIST_RE = re.compile(
    r"\[(\d+:\d+:\d+\.\d+),?\d*\].*Distance:\s+([-\d.]+)\s+m"
    r"(?:\s+\(tag=0x([0-9A-Fa-f]+)\))?"
)


class DistanceResource(resource.Resource):
    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        global _dist_count, _t_last_dist

        payload = request.payload
        if len(payload) == DIST_FMT_V2.size:
            (anchor_id, tag_id, distance_mm, uptime_s,
             rssi_q8, fp_power_q8, fp_index, peak_index) = DIST_FMT_V2.unpack(payload)
            rssi_dbm = rssi_q8 / 256.0
            fp_dbm = fp_power_q8 / 256.0
            fp_idx = fp_index / 64.0  # Q10.6
            diag = f"  rssi={rssi_dbm:.1f}dBm  fp={fp_dbm:.1f}dBm  fp_idx={fp_idx:.1f}  peak={peak_index}"
            nlos_level, nlos_tag = _nlos_flag(rssi_dbm, fp_dbm, fp_idx, peak_index)
            diag += nlos_tag
        elif len(payload) == DIST_FMT_V1.size:
            anchor_id, tag_id, distance_mm, uptime_s = DIST_FMT_V1.unpack(payload)
            diag = ""
            nlos_level = 0
        else:
            print(f"[WARN] /distance bad size {len(payload)}B")
            return aiocoap.Message(code=aiocoap.CHANGED)

        distance_m = distance_mm / 1000.0

        now  = time.monotonic()
        pair = (anchor_id, tag_id)
        prev = _t_last_dist.get(pair)
        rate = f"  ({1/(now - prev):.1f} Hz)" if prev else ""
        _t_last_dist[pair] = now
        _dist_count += 1

        src = request.remote.hostinfo if request.remote else "?"
        line = (
            f"[DIST {_dist_count:>4}]  anchor=0x{anchor_id:04X}  tag=0x{tag_id:04X}"
            f"  dist={distance_m:7.3f} m  uptime={uptime_s:6d}s"
            f"  from={src}{rate}{diag}"
        )
        if nlos_level == 2:
            print(f"{_RED}{line}{_RESET}")
        elif nlos_level == 1:
            print(f"{_YELLOW}{line}{_RESET}")
        else:
            print(line)
        return aiocoap.Message(code=aiocoap.CHANGED)


class EventResource(resource.Resource):
    def __init__(self, quiet=False):
        super().__init__()
        self._quiet = quiet

    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        global _evt_count

        payload = request.payload
        if len(payload) != EVT_SIZE:
            if not self._quiet:
                print(f"[WARN] /event bad size {len(payload)}B")
            return aiocoap.Message(code=aiocoap.CHANGED)

        node_id, event, seq = EVT_FMT.unpack(payload)
        evt_name = EVT_NAMES.get(event, f"0x{event:02X}")

        _evt_count += 1
        if not self._quiet:
            src = request.remote.hostinfo if request.remote else "?"
            line = (
                f"[EVT  {_evt_count:>4}]  node=0x{node_id:04X}  {evt_name:<10}"
                f"  seq={seq:3d}  from={src}"
            )
            if event == 0x20:  # DISC_RX
                print(f"{_CYAN}{line}{_RESET}")
            else:
                print(line)
        return aiocoap.Message(code=aiocoap.CHANGED)


class CIRResource(resource.Resource):
    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        global _cir_count

        payload = request.payload
        if len(payload) < CIR_HDR_FMT.size:
            print(f"[WARN] /cir bad size {len(payload)}B")
            return aiocoap.Message(code=aiocoap.CHANGED)

        (anchor_id, tag_id, distance_mm, uptime_s,
         fp_index, sample_offset, num_samples, read_mode) = CIR_HDR_FMT.unpack(
            payload[:CIR_HDR_FMT.size])

        distance_m = distance_mm / 1000.0
        _cir_count += 1

        src = request.remote.hostinfo if request.remote else "?"
        print(
            f"[CIR  {_cir_count:>4}]  anchor=0x{anchor_id:04X}  tag=0x{tag_id:04X}"
            f"  {num_samples} samples @ offset={sample_offset}"
            f"  dist={distance_m:.3f} m  from={src}"
        )
        return aiocoap.Message(code=aiocoap.CHANGED)


async def serial_reader(port, baud=115200):
    """Read distance log lines from a serial port."""
    global _dist_count, _t_last_dist

    import serial

    loop = asyncio.get_event_loop()
    ser = serial.Serial(port, baud, timeout=0.1)
    ser.reset_input_buffer()
    print(f"Serial: reading from {port} @ {baud}")

    try:
        while True:
            # Non-blocking readline in executor to avoid blocking the event loop
            line = await loop.run_in_executor(None, ser.readline)
            if not line:
                continue
            try:
                text = line.decode("utf-8", errors="replace").strip()
                text = _ANSI_RE.sub("", text)  # strip ANSI escape codes
            except Exception:
                continue

            m = _SERIAL_DIST_RE.search(text)
            if not m:
                continue

            uptime_str = m.group(1)
            distance_m = float(m.group(2))
            tag_hex = m.group(3)
            tag_id = int(tag_hex, 16) if tag_hex else 0

            # Parse uptime HH:MM:SS.mmm
            parts = uptime_str.split(":")
            uptime_s = int(parts[0]) * 3600 + int(parts[1]) * 60
            sec_parts = parts[2].split(".")
            uptime_s += int(sec_parts[0])

            now = time.monotonic()
            pair = (0x0002, tag_id)
            prev = _t_last_dist.get(pair)
            rate = f"  ({1/(now - prev):.1f} Hz)" if prev else ""
            _t_last_dist[pair] = now
            _dist_count += 1

            print(
                f"[DIST {_dist_count:>4}]  anchor=0x0002  tag=0x{tag_id:04X}"
                f"  dist={distance_m:7.3f} m  uptime={uptime_s:6d}s"
                f"  from={port}{rate}"
            )
    except asyncio.CancelledError:
        pass
    finally:
        ser.close()


async def main():
    parser = argparse.ArgumentParser(description="UWB CoAP monitor")
    parser.add_argument("-d", "--distance-only", action="store_true",
                        help="show only anchor distance messages, suppress tag events")
    parser.add_argument("-s", "--serial", metavar="PORT",
                        help="read distance logs from serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("-b", "--baud", type=int, default=115200,
                        help="serial baud rate (default: 115200)")
    args = parser.parse_args()

    mode = "distance only" if args.distance_only else "distance + events"
    sources = "CoAP"
    if args.serial:
        sources += f" + serial ({args.serial})"
    print("UWB Monitor")
    print(f"Listening on [::]:5683  ({mode}, {sources})")
    print("Press Ctrl-C to stop.\n")

    site = resource.Site()
    site.add_resource(["distance"], DistanceResource())
    site.add_resource(["event"],    EventResource(quiet=args.distance_only))
    site.add_resource(["cir"],      CIRResource())

    context = await aiocoap.Context.create_server_context(site, bind=("::", 5683))

    serial_task = None
    if args.serial:
        serial_task = asyncio.create_task(serial_reader(args.serial, args.baud))

    try:
        await asyncio.get_event_loop().create_future()
    except asyncio.CancelledError:
        pass
    finally:
        if serial_task:
            serial_task.cancel()
            try:
                await serial_task
            except asyncio.CancelledError:
                pass
        await context.shutdown()
        print(f"\nTotal: {_dist_count} distance, {_evt_count} events, {_cir_count} CIR captures.")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
