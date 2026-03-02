#!/bin/bash
# =============================================================
# UWB Mesh Tracker - Firmware Build Script
#
# Usage:
#   ./build.sh [BOARD] [--flash] [--clean]
#
# Examples:
#   ./build.sh                                  # Build for nRF52840 DK (anchor)
#   ./build.sh decawave_dwm3001cdk              # Build for DWM3001CDK (tag)
#   ./build.sh xiao_ble                         # Build for XIAO BLE (tag)
#   ./build.sh nrf52840dk/nrf52840 --flash      # Build and flash nRF52840 DK
#   ./build.sh decawave_dwm3001cdk --flash      # Build and flash DWM3001CDK
#   ./build.sh nrf52840dk/nrf52840 --clean      # Clean rebuild
#
# Board → Default Role:
#   nrf52840dk/nrf52840     → ANCHOR (addr 0x0001)
#   decawave_dwm3001cdk     → TAG    (addr 0x0100)
#   xiao_ble                → TAG    (addr 0x0200)
#
# To build as TAG on nRF52840DK, add:
#   -- -DCONFIG_NODE_ROLE_TAG=y -DCONFIG_NODE_ROLE_ANCHOR=n \
#      -DCONFIG_UWB_NODE_SHORT_ADDR=0x0100
# =============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# nRF Connect SDK installed by nrfutil (preferred over capstone/v3.2.2 copy)
SDK_DIR="${HOME}/ncs/v3.2.2"
TOOLCHAIN="${HOME}/ncs/toolchains/927563c840"
APP_DIR="${SCRIPT_DIR}"
BOARD="${1:-nrf52840dk/nrf52840}"
DO_FLASH=false
DO_CLEAN=false

# Set up toolchain environment
export PATH="${TOOLCHAIN}/bin:${TOOLCHAIN}/usr/bin:${TOOLCHAIN}/usr/local/bin:${TOOLCHAIN}/opt/bin:${TOOLCHAIN}/opt/zephyr-sdk/arm-zephyr-eabi/bin:/usr/bin:/bin"
export LD_LIBRARY_PATH="${TOOLCHAIN}/lib:${TOOLCHAIN}/lib/x86_64-linux-gnu:${TOOLCHAIN}/usr/local/lib"
export PYTHONHOME="${TOOLCHAIN}/usr/local"
export PYTHONPATH="${TOOLCHAIN}/usr/local/lib/python3.12:${TOOLCHAIN}/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${TOOLCHAIN}/opt/zephyr-sdk"

# Parse flags
for arg in "$@"; do
    case "$arg" in
        --flash)  DO_FLASH=true ;;
        --clean)  DO_CLEAN=true ;;
    esac
done

# Sanitize board name for directory (replace / with -)
BUILD_SUBDIR="${BOARD//\//-}"
BUILD_DIR="${APP_DIR}/build/${BUILD_SUBDIR}"

# Validate SDK exists
if [ ! -d "${SDK_DIR}" ]; then
    echo "ERROR: nRF Connect SDK not found at ${SDK_DIR}"
    echo "       Expected the v3.2.2/ directory to be a sibling of firmware/"
    exit 1
fi

# Source the SDK environment if nrfutil is available
if command -v nrfutil &>/dev/null; then
    echo "[build.sh] nrfutil found: $(nrfutil --version)"
fi

echo "=== UWB Mesh Tracker Build ==="
echo "Board:     ${BOARD}"
echo "Build dir: ${BUILD_DIR}"
echo "SDK:       ${SDK_DIR}"
echo ""

# Clean if requested
if [ "$DO_CLEAN" = true ] && [ -d "${BUILD_DIR}" ]; then
    echo "[build.sh] Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# Run west build from inside the SDK workspace
cd "${SDK_DIR}"

EXTRA_ARGS="-DZEPHYR_EXTRA_MODULES=${SCRIPT_DIR}/../zephyr-dw3000-decadriver"

# DWM3001CDK (nRF52833, 512KB) needs single-slot MCUboot + serial recovery
if [[ "$BOARD" == *"dwm3001"* ]]; then
    EXTRA_ARGS="$EXTRA_ARGS -DSB_OVERLAY_CONFIG=${APP_DIR}/sysbuild_dwm3001cdk.conf"
    EXTRA_ARGS="$EXTRA_ARGS -Dmcuboot_OVERLAY_CONFIG=${APP_DIR}/sysbuild/mcuboot_dwm3001cdk/serial_recovery.conf"
    EXTRA_ARGS="$EXTRA_ARGS -Dmcuboot_DTC_OVERLAY_FILE=${APP_DIR}/sysbuild/mcuboot_dwm3001cdk/serial_recovery.overlay"
fi

# XIAO BLE: app-only build (no MCUboot, uses stock UF2 bootloader)
if [[ "$BOARD" == "xiao_ble" ]]; then
    NO_SYSBUILD="--no-sysbuild"
fi

west build \
    --board "${BOARD}" \
    --source-dir "${APP_DIR}" \
    --build-dir "${BUILD_DIR}" \
    ${NO_SYSBUILD:-} \
    -- \
    $EXTRA_ARGS

echo ""
echo "=== Build successful ==="

if [[ "$BOARD" == "xiao_ble" ]]; then
    # XIAO BLE: app-only build — convert zephyr.hex to UF2 for drag-and-drop flash
    UF2_SCRIPT="${SDK_DIR}/zephyr/scripts/build/uf2conv.py"
    "${TOOLCHAIN}/usr/local/bin/python3" "${UF2_SCRIPT}" -c -f 0xada52840 \
        -o "${BUILD_DIR}/zephyr/zephyr.uf2" "${BUILD_DIR}/zephyr/zephyr.hex"
    echo "Flash image: ${BUILD_DIR}/zephyr/zephyr.hex"
    echo "UF2 image:   ${BUILD_DIR}/zephyr/zephyr.uf2  (drag to XIAO USB drive)"
else
    echo "Flash image: ${BUILD_DIR}/merged.hex"
    echo "OTA image:   ${BUILD_DIR}/firmware/zephyr/zephyr.signed.bin"
fi

# Flash if requested
if [ "$DO_FLASH" = true ]; then
    echo ""
    if [[ "$BOARD" == "xiao_ble" ]]; then
        echo "=== XIAO BLE: drag-and-drop flash ==="
        echo "1. Double-tap RST to enter UF2 bootloader"
        echo "2. Drag ${BUILD_DIR}/zephyr/zephyr.uf2 to the XIAO USB drive"
    else
        echo "[build.sh] Flashing ${BOARD}..."
        # Use merged.hex which includes MCUboot + signed app
        unset PYTHONHOME PYTHONPATH
        nrfutil device program --firmware "${BUILD_DIR}/merged.hex"
        echo "=== Flash complete ==="
    fi
fi
