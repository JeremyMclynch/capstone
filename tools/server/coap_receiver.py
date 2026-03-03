#!/usr/bin/env python3
"""
Standalone CoAP distance receiver for Thread testing.

Listens on UDP port 5683 for CoAP POST /distance from the anchor.
Joins the ff03::1 multicast group on wpan0 so multicast packets arrive.

Binary payload (12 bytes, little-endian):
  [0-1]  anchor_id   (uint16)
  [2-3]  tag_id      (uint16)
  [4-7]  distance_mm (uint32)
  [8-11] uptime_s    (uint32)
"""

import asyncio
import logging
import socket
import struct
import sys

import aiocoap
import aiocoap.resource as resource

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

PAYLOAD_FMT  = struct.Struct("<HHIi")
THREAD_IFACE = "wpan0"
MCAST_ADDR   = "ff03::1"
COAP_PORT    = 5683


def join_multicast(iface: str, group: str) -> bool:
    """Join an IPv6 multicast group on the given interface."""
    try:
        ifindex = socket.if_nametoindex(iface)
    except OSError as e:
        log.warning("Cannot get index for %s: %s", iface, e)
        return False

    # Convert group address to packed bytes
    packed = socket.inet_pton(socket.AF_INET6, group)
    # struct ipv6_mreq: in6_addr + uint32_t ifindex
    mreq = packed + struct.pack("I", ifindex)

    try:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)
        sock.close()
        log.info("Joined multicast %s on %s (ifindex=%d)", group, iface, ifindex)
        return True
    except OSError as e:
        log.warning("Multicast join failed: %s (may still receive unicast)", e)
        return False


class DistanceResource(resource.Resource):
    """CoAP /distance resource — accepts POST with the 12-byte payload."""

    received = 0

    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        payload = request.payload

        if len(payload) != PAYLOAD_FMT.size:
            log.warning("Bad payload size: %d (want %d)", len(payload), PAYLOAD_FMT.size)
            return aiocoap.Message(code=aiocoap.BAD_REQUEST)

        anchor_id, tag_id, distance_mm, uptime_s = PAYLOAD_FMT.unpack(payload)
        distance_m = distance_mm / 1000.0

        DistanceResource.received += 1
        log.info(
            "[#%d] anchor=0x%04X  tag=0x%04X  dist=%.3f m  uptime=%ds  from=%s",
            DistanceResource.received,
            anchor_id, tag_id, distance_m, uptime_s,
            request.remote,
        )
        return aiocoap.Message(code=aiocoap.CHANGED)


async def main() -> None:
    # Join multicast so we receive ff03::1 packets
    join_multicast(THREAD_IFACE, MCAST_ADDR)

    site = resource.Site()
    site.add_resource(["distance"], DistanceResource())

    context = await aiocoap.Context.create_server_context(site, bind=("::", COAP_PORT))
    log.info("CoAP server listening on [::]:%d — waiting for distance data...", COAP_PORT)
    log.info("Anchor sends to [%s]:%d/distance", MCAST_ADDR, COAP_PORT)

    try:
        await asyncio.get_running_loop().create_future()  # run forever
    except asyncio.CancelledError:
        pass
    finally:
        await context.shutdown()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("Stopped.")
