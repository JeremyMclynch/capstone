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
#   nrf52840dk/nrf52840               → ANCHOR (addr 0x0001)
#   decawave_dwm3001cdk               → TAG    (addr 0x0100)
#   xiao_ble                          → TAG    (addr 0x0200)
#
# To build as TAG on nRF52840DK, add:
#   -- -DCONFIG_NODE_ROLE_TAG=y -DCONFIG_NODE_ROLE_ANCHOR=n \
#      -DCONFIG_UWB_NODE_SHORT_ADDR=0x0100
# =============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN="${HOME}/ncs/toolchains/927563c840"
APP_DIR="${SCRIPT_DIR}"
BOARD="${1:-nrf52840dk/nrf52840}"
DO_FLASH=false
DO_CLEAN=false

# Detect west workspace
# T2 topology: .west/ is in the parent of the project root (standard west init -l)
# Flat layout: .west/ is inside the project root (manual setup)
USE_WORKSPACE=false
WEST_TOPDIR=""
if [ -d "${PROJECT_ROOT}/.west" ] && [ -f "${PROJECT_ROOT}/west.yml" ]; then
    USE_WORKSPACE=true
    WEST_TOPDIR="${PROJECT_ROOT}"
elif [ -d "$(dirname "${PROJECT_ROOT}")/.west" ] && [ -f "${PROJECT_ROOT}/west.yml" ]; then
    USE_WORKSPACE=true
    WEST_TOPDIR="$(dirname "${PROJECT_ROOT}")"
fi

# Parse flags
for arg in "$@"; do
    case "$arg" in
        --flash)  DO_FLASH=true ;;
        --clean)  DO_CLEAN=true ;;
    esac
done

# Set up nRF Connect SDK toolchain environment
export PATH="${TOOLCHAIN}/bin:${TOOLCHAIN}/usr/bin:${TOOLCHAIN}/usr/local/bin:${TOOLCHAIN}/opt/bin:${TOOLCHAIN}/opt/zephyr-sdk/arm-zephyr-eabi/bin:/usr/bin:/bin"
export LD_LIBRARY_PATH="${TOOLCHAIN}/lib:${TOOLCHAIN}/lib/x86_64-linux-gnu:${TOOLCHAIN}/usr/local/lib"
export PYTHONHOME="${TOOLCHAIN}/usr/local"
export PYTHONPATH="${TOOLCHAIN}/usr/local/lib/python3.12:${TOOLCHAIN}/usr/local/lib/python3.12/site-packages"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${TOOLCHAIN}/opt/zephyr-sdk"

# Sanitize board name for directory (replace / with -)
BUILD_SUBDIR="${BOARD//\//-}"
BUILD_DIR="${APP_DIR}/build/${BUILD_SUBDIR}"

# Resolve west workspace or fallback SDK
if [ "$USE_WORKSPACE" = true ]; then
    WEST_DIR="${WEST_TOPDIR}"
else
    SDK_DIR="${HOME}/ncs/v3.2.2"
    WEST_DIR="${SDK_DIR}"
    if [ ! -d "${SDK_DIR}" ]; then
        echo "ERROR: nRF Connect SDK not found."
        echo "Set up a west workspace:"
        echo "  mkdir workspace && cd workspace"
        echo "  git clone <repo-url> capstone"
        echo "  west init -l capstone && west update"
        echo "Or install the SDK via nrfutil."
        exit 1
    fi
fi

# Check nrfutil
if command -v nrfutil &>/dev/null; then
    echo "[build.sh] nrfutil found: $(nrfutil --version)"
fi

echo "=== UWB Mesh Tracker Build ==="
echo "Board:     ${BOARD}"
echo "Build dir: ${BUILD_DIR}"
echo "Workspace: ${WEST_DIR} ($([ "$USE_WORKSPACE" = true ] && echo 'west manifest' || echo 'nrfutil SDK'))"
echo ""

# Clean if requested
if [ "$DO_CLEAN" = true ] && [ -d "${BUILD_DIR}" ]; then
    echo "[build.sh] Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# Run west build from inside the workspace
cd "${WEST_DIR}"

EXTRA_ARGS=""
if [ "$USE_WORKSPACE" = false ]; then
    EXTRA_ARGS="-DZEPHYR_EXTRA_MODULES=${PROJECT_ROOT}/zephyr-dw3000-decadriver"
fi

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
    # XIAO BLE: app-only build — convert zephyr.hex to UF2 and generate DFU package
    UF2_SCRIPT="${WEST_DIR}/zephyr/scripts/build/uf2conv.py"
    "${TOOLCHAIN}/usr/local/bin/python3" "${UF2_SCRIPT}" -c -f 0xada52840 \
        -o "${BUILD_DIR}/zephyr/zephyr.uf2" "${BUILD_DIR}/zephyr/zephyr.hex"

    # Generate DFU package for serial flashing via adafruit-nrfutil
    NRFUTIL_BIN="${HOME}/.local/bin/adafruit-nrfutil"
    if [ -x "$NRFUTIL_BIN" ]; then
        unset PYTHONHOME PYTHONPATH
        "$NRFUTIL_BIN" dfu genpkg \
            --dev-type 0x0052 \
            --application "${BUILD_DIR}/zephyr/zephyr.hex" \
            "${BUILD_DIR}/zephyr/dfu_package.zip" 2>/dev/null
        echo "Flash image: ${BUILD_DIR}/zephyr/zephyr.hex"
        echo "UF2 image:   ${BUILD_DIR}/zephyr/zephyr.uf2  (drag to XIAO USB drive)"
        echo "DFU package: ${BUILD_DIR}/zephyr/dfu_package.zip  (serial flash)"
    else
        echo "Flash image: ${BUILD_DIR}/zephyr/zephyr.hex"
        echo "UF2 image:   ${BUILD_DIR}/zephyr/zephyr.uf2  (drag to XIAO USB drive)"
        echo "(install adafruit-nrfutil for serial DFU flashing)"
    fi
else
    echo "Flash image: ${BUILD_DIR}/merged.hex"
    echo "OTA image:   ${BUILD_DIR}/firmware/zephyr/zephyr.signed.bin"
fi

# Flash if requested
if [ "$DO_FLASH" = true ]; then
    echo ""
    if [[ "$BOARD" == "xiao_ble" ]]; then
        NRFUTIL_BIN="${HOME}/.local/bin/adafruit-nrfutil"
        if [ ! -x "$NRFUTIL_BIN" ]; then
            echo "ERROR: adafruit-nrfutil not found. Install with:"
            echo "  python3 -m venv ~/.local/share/adafruit-nrfutil-venv"
            echo "  ~/.local/share/adafruit-nrfutil-venv/bin/pip install adafruit-nrfutil"
            echo "  ln -sf ~/.local/share/adafruit-nrfutil-venv/bin/adafruit-nrfutil ~/.local/bin/"
            exit 1
        fi

        # Auto-detect XIAO serial port from nrfutil device list
        unset PYTHONHOME PYTHONPATH
        XIAO_PORT=$(nrfutil device list 2>/dev/null | grep -A2 "XIAO" | grep -oP '/dev/ttyACM\d+' | head -1)
        if [[ -z "$XIAO_PORT" ]]; then
            echo "ERROR: No XIAO device found. Double-tap RST to enter bootloader, then re-run."
            exit 1
        fi

        echo "[build.sh] Flashing XIAO BLE via serial DFU on ${XIAO_PORT}..."
        "$NRFUTIL_BIN" --verbose dfu serial \
            --package "${BUILD_DIR}/zephyr/dfu_package.zip" \
            --port "$XIAO_PORT" \
            --baudrate 115200 \
            --singlebank \
            --touch 1200
        echo "=== Flash complete ==="
    else
        echo "[build.sh] Flashing ${BOARD}..."
        # Use merged.hex which includes MCUboot + signed app
        unset PYTHONHOME PYTHONPATH
        nrfutil device program --firmware "${BUILD_DIR}/merged.hex"
        echo "=== Flash complete ==="
    fi
fi
