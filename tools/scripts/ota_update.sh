#!/usr/bin/env bash
# ota_update.sh — Upload firmware to a device over Thread (MCUmgr SMP/UDP)
#
# Only works on anchor (nRF52840, dual-slot MCUboot with swap).
# Tag (DWM3001CDK, 512KB) uses single-slot MCUboot — OTA not supported (flash too small).
#
# Usage:  ./scripts/ota_update.sh <device-ipv6-addr> <firmware.bin>
#
# Examples:
#   ./scripts/ota_update.sh fdde:ad00:beef:0:a firmware/build/nrf52840dk-nrf52840/firmware/zephyr/zephyr.signed.bin
#
# Requires: mcumgr CLI (go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest)

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <device-ipv6-addr> <firmware.bin>"
    echo ""
    echo "  device-ipv6-addr  Mesh-local IPv6 address (no brackets)"
    echo "  firmware.bin      Signed update image (zephyr.signed.bin from build)"
    echo ""
    echo "Find device addresses with:  sudo ot-ctl neighbor table"
    exit 1
fi

ADDR="$1"
IMAGE="$2"
CONNSTRING="[${ADDR}]:1337"

command -v mcumgr >/dev/null 2>&1 || die "mcumgr CLI not found. Install: go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest"
[[ -f "$IMAGE" ]] || die "Firmware image not found: $IMAGE"

echo "=== OTA Firmware Update ==="
echo "Target:  ${ADDR}"
echo "Image:   ${IMAGE}"
echo ""

# 1. Upload firmware image
echo "Uploading firmware image..."
mcumgr --conntype udp --connstring="${CONNSTRING}" image upload "$IMAGE"
echo ""

# 2. List images to verify upload
echo "Verifying uploaded images..."
mcumgr --conntype udp --connstring="${CONNSTRING}" image list
echo ""

# 3. Reset device to boot into new image
echo "Resetting device..."
mcumgr --conntype udp --connstring="${CONNSTRING}" reset || true
echo ""

echo "=== OTA update complete ==="
echo "Device is rebooting with the new firmware."
echo ""
echo "For dual-slot devices (anchor), the new image is pending test."
echo "To confirm permanently:  mcumgr --conntype udp --connstring=${CONNSTRING} image confirm"
