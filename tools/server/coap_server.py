"""
CoAP server resource handler for UWB distance measurements.

Listens for POST requests on the /distance resource.

Binary payload format (12 bytes, little-endian):
    Offset  Size  Field
    0       2     anchor_id   (uint16)
    2       2     tag_id      (uint16)
    4       4     distance_mm (uint32, millimeters)
    8       4     uptime_s    (uint32, seconds since node boot)
"""

import logging
import struct

import aiocoap
import aiocoap.resource as resource

from database import insert_measurement

logger = logging.getLogger(__name__)

# Payload struct: 2B anchor, 2B tag, 4B distance_mm, 4B uptime_s
PAYLOAD_STRUCT = struct.Struct("<HHII")
PAYLOAD_SIZE   = PAYLOAD_STRUCT.size  # 12 bytes


class DistanceResource(resource.Resource):
    """CoAP resource at /distance - accepts POST with distance payload."""

    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        payload = request.payload

        if len(payload) != PAYLOAD_SIZE:
            logger.warning(
                "Unexpected payload size: %d bytes (expected %d)",
                len(payload), PAYLOAD_SIZE,
            )
            return aiocoap.Message(
                code=aiocoap.BAD_REQUEST,
                payload=b"Bad payload size",
            )

        anchor_id, tag_id, distance_mm, uptime_s = PAYLOAD_STRUCT.unpack(payload)
        distance_m = distance_mm / 1000.0

        logger.info(
            "Measurement: anchor=0x%04X tag=0x%04X dist=%.3f m (uptime=%ds)",
            anchor_id, tag_id, distance_m, uptime_s,
        )

        # Sanity-check distance (UWB range: ~0.1 m to ~100 m)
        if not (0.0 < distance_m < 200.0):
            logger.warning("Distance %.3f m out of plausible range, storing anyway",
                           distance_m)

        rowid = insert_measurement(
            anchor_id=anchor_id,
            tag_id=tag_id,
            distance_mm=distance_mm,
            node_uptime_s=uptime_s,
        )
        logger.debug("Stored measurement id=%d", rowid)

        return aiocoap.Message(code=aiocoap.CHANGED)


def build_site() -> resource.Site:
    """Create and return the CoAP resource tree."""
    site = resource.Site()
    site.add_resource(["distance"], DistanceResource())
    return site
