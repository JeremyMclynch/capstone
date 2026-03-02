#!/usr/bin/env python3
"""
UWB monitor — receives CoAP POSTs from anchor (distance) and tag (events).

Usage:
    python3 monitor.py

Resources:
  POST /distance  — from anchor, 12-byte distance measurement
  POST /event     — from tag,    6-byte UWB packet event

No dependencies beyond aiocoap:
    pip install aiocoap
"""

import argparse
import asyncio
import struct
import time
import aiocoap
import aiocoap.resource as resource

# /distance payload: anchor_id (u16), tag_id (u16), distance_mm (u32), uptime_s (u32)
DIST_FMT  = struct.Struct("<HHII")
DIST_SIZE = DIST_FMT.size  # 12 bytes

# /event payload: node_id (u16), event (u8), seq (u8), reserved (u16)
EVT_FMT  = struct.Struct("<HBBxx")
EVT_SIZE = EVT_FMT.size   # 6 bytes

EVT_NAMES = {
    0x01: "POLL_TX",
    0x02: "RESP_RX",
    0x03: "FINAL_TX",
    0x10: "NO_RESP",
}

_dist_count = 0
_evt_count  = 0
_t_last_dist = None


class DistanceResource(resource.Resource):
    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        global _dist_count, _t_last_dist

        payload = request.payload
        if len(payload) != DIST_SIZE:
            print(f"[WARN] /distance bad size {len(payload)}B")
            return aiocoap.Message(code=aiocoap.CHANGED)

        anchor_id, tag_id, distance_mm, uptime_s = DIST_FMT.unpack(payload)
        distance_m = distance_mm / 1000.0

        now  = time.monotonic()
        rate = f"  ({1/(now - _t_last_dist):.1f} Hz)" if _t_last_dist else ""
        _t_last_dist = now
        _dist_count += 1

        src = request.remote.hostinfo if request.remote else "?"
        print(
            f"[DIST {_dist_count:>4}]  anchor=0x{anchor_id:04X}  tag=0x{tag_id:04X}"
            f"  dist={distance_m:7.3f} m  uptime={uptime_s:6d}s"
            f"  from={src}{rate}"
        )
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
            print(
                f"[EVT  {_evt_count:>4}]  node=0x{node_id:04X}  {evt_name:<10}"
                f"  seq={seq:3d}  from={src}"
            )
        return aiocoap.Message(code=aiocoap.CHANGED)


async def main():
    parser = argparse.ArgumentParser(description="UWB CoAP monitor")
    parser.add_argument("-d", "--distance-only", action="store_true",
                        help="show only anchor distance messages, suppress tag events")
    args = parser.parse_args()

    mode = "distance only" if args.distance_only else "distance + events"
    print("UWB Monitor")
    print(f"Listening on [::]:5683  ({mode})")
    print("Press Ctrl-C to stop.\n")

    site = resource.Site()
    site.add_resource(["distance"], DistanceResource())
    site.add_resource(["event"],    EventResource(quiet=args.distance_only))

    context = await aiocoap.Context.create_server_context(site, bind=("::", 5683))

    try:
        await asyncio.get_event_loop().create_future()
    except asyncio.CancelledError:
        pass
    finally:
        await context.shutdown()
        print(f"\nTotal: {_dist_count} distance measurements, {_evt_count} tag events.")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
