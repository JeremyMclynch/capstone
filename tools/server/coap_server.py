"""
CoAP server resource handler for UWB distance measurements.

Listens for POST requests on the /distance resource.

Binary payload format (20 bytes, little-endian; 12-byte legacy also accepted):
    Offset  Size  Field
    0       2     anchor_id    (uint16)
    2       2     tag_id       (uint16)
    4       4     distance_mm  (uint32, millimeters)
    8       4     uptime_s     (uint32, seconds since node boot)
    12      2     rssi_q8      (int16, Q8.8 dBm — divide by 256)
    14      2     fp_power_q8  (int16, Q8.8 dBm — divide by 256)
    16      2     fp_index     (uint16, Q10.6 — divide by 64)
    18      2     peak_index   (uint16)
"""

import logging
import struct

import aiocoap
import aiocoap.resource as resource

from database import insert_measurement

logger = logging.getLogger(__name__)

PAYLOAD_V2 = struct.Struct("<HHIIhhHH")  # 20 bytes
PAYLOAD_V1 = struct.Struct("<HHII")       # 12 bytes (legacy)


class DistanceResource(resource.Resource):
    """CoAP resource at /distance - accepts POST with distance payload."""

    async def render_post(self, request: aiocoap.Message) -> aiocoap.Message:
        payload = request.payload

        rssi_dbm = None
        fp_power_dbm = None
        fp_index = None
        peak_index = None

        if len(payload) == PAYLOAD_V2.size:
            (anchor_id, tag_id, distance_mm, uptime_s,
             rssi_q8, fp_power_q8, fp_idx_raw, peak_idx) = PAYLOAD_V2.unpack(payload)
            rssi_dbm = rssi_q8 / 256.0
            fp_power_dbm = fp_power_q8 / 256.0
            fp_index = fp_idx_raw / 64.0
            peak_index = peak_idx
        elif len(payload) == PAYLOAD_V1.size:
            anchor_id, tag_id, distance_mm, uptime_s = PAYLOAD_V1.unpack(payload)
        else:
            logger.warning(
                "Unexpected payload size: %d bytes (expected %d or %d)",
                len(payload), PAYLOAD_V2.size, PAYLOAD_V1.size,
            )
            return aiocoap.Message(
                code=aiocoap.BAD_REQUEST,
                payload=b"Bad payload size",
            )

        distance_m = distance_mm / 1000.0

        logger.info(
            "Measurement: anchor=0x%04X tag=0x%04X dist=%.3f m (uptime=%ds)"
            + (f" rssi={rssi_dbm:.1f}dBm fp={fp_power_dbm:.1f}dBm" if rssi_dbm is not None else ""),
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
            rssi_dbm=rssi_dbm,
            fp_power_dbm=fp_power_dbm,
            fp_index=fp_index,
            peak_index=peak_index,
        )
        logger.debug("Stored measurement id=%d", rowid)

        return aiocoap.Message(code=aiocoap.CHANGED)


def build_site() -> resource.Site:
    """Create and return the CoAP resource tree."""
    site = resource.Site()
    site.add_resource(["distance"], DistanceResource())
    return site
