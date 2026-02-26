#!/bin/bash
# =============================================================
# Build firmware for multiple nodes with different addresses
#
# Builds the firmware for each anchor and tag in your deployment.
# Edit the arrays below to match your node count and addresses.
# =============================================================

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="${HOME}/ncs/v3.2.2"
TOOLCHAIN="${HOME}/ncs/toolchains/927563c840"

export PATH="${TOOLCHAIN}/bin:${TOOLCHAIN}/usr/bin:${TOOLCHAIN}/usr/local/bin:${TOOLCHAIN}/opt/bin:${TOOLCHAIN}/opt/zephyr-sdk/arm-zephyr-eabi/bin:/usr/bin:/bin"
export LD_LIBRARY_PATH="${TOOLCHAIN}/lib:${TOOLCHAIN}/lib/x86_64-linux-gnu:${TOOLCHAIN}/usr/local/lib"
export PYTHONHOME="${TOOLCHAIN}/usr/local"
export PYTHONPATH="${TOOLCHAIN}/usr/local/lib/python3.12:${TOOLCHAIN}/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${TOOLCHAIN}/opt/zephyr-sdk"

build_node() {
    local BOARD="$1"
    local ROLE="$2"          # ANCHOR or TAG
    local ADDR="$3"          # hex, e.g. 0x0001
    local BUILD_TAG="$4"     # label for build dir, e.g. anchor1

    local BUILD_DIR="${SCRIPT_DIR}/build/${BUILD_TAG}"

    echo ""
    echo "--- Building ${BUILD_TAG}: board=${BOARD} role=${ROLE} addr=${ADDR} ---"

    cd "${SDK_DIR}"
    west build \
        --board "${BOARD}" \
        --source-dir "${SCRIPT_DIR}" \
        --build-dir "${BUILD_DIR}" \
        -- \
        -DZEPHYR_EXTRA_MODULES="${SCRIPT_DIR}/../zephyr-dw3000-decadriver" \
        -DCONFIG_NODE_ROLE_${ROLE}=y \
        -DCONFIG_UWB_NODE_SHORT_ADDR="${ADDR}"

    echo "  → ${BUILD_DIR}/zephyr/zephyr.hex"
}

# ── Anchors (nRF52840 DK + DWM3000EVB) ──────────────────────────────
build_node "nrf52840dk/nrf52840" "ANCHOR" "0x0001" "anchor1"
build_node "nrf52840dk/nrf52840" "ANCHOR" "0x0002" "anchor2"
build_node "nrf52840dk/nrf52840" "ANCHOR" "0x0003" "anchor3"

# ── Tags (DWM3001CDK) ────────────────────────────────────────────────
build_node "decawave_dwm3001cdk" "TAG"    "0x0100" "tag1"
build_node "decawave_dwm3001cdk" "TAG"    "0x0101" "tag2"

echo ""
echo "=== All builds complete ==="
