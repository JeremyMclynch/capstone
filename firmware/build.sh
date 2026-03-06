#!/bin/bash
# =============================================================
# UWB Mesh Tracker - Firmware Build Script
#
# Usage:
#   ./build.sh [BOARD] [--flash] [--flash-jlink] [--clean]
#
# Examples:
#   ./build.sh                                  # Build for nRF52840 DK (anchor)
#   ./build.sh decawave_dwm3001cdk              # Build for DWM3001CDK (tag)
#   ./build.sh xiao_ble                         # Build for XIAO BLE (anchor)
#   OTA_TARGET=<ipv6> ./build.sh nrf52840dk/nrf52840 --flash  # Build and OTA
#   OTA_TARGET=<ipv6> ./build.sh xiao_ble --flash            # Build and OTA
#   ./build.sh xiao_ble --flash-jlink           # Build and flash XIAO via J-Link SWD
#   ./build.sh nrf52840dk/nrf52840 --flash-jlink # Build and flash DK via J-Link
#   ./build.sh nrf52840dk/nrf52840 --clean      # Clean rebuild
#
# Flash modes:
#   --flash       OTA update via MCUmgr (uploads signed bin over Thread)
#   --flash-jlink Full flash via J-Link (writes merged.hex including MCUboot)
#
# Board → Default Role:
#   nrf52840dk/nrf52840               → ANCHOR (addr auto)
#   decawave_dwm3001cdk               → TAG    (addr auto)
#   xiao_ble                          → ANCHOR (addr auto)
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
DO_FLASH_JLINK=false
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
        --flash)       DO_FLASH=true ;;
        --flash-jlink) DO_FLASH_JLINK=true ;;
        --clean)       DO_CLEAN=true ;;
    esac
done

# Set up nRF Connect SDK toolchain environment
export PATH="${TOOLCHAIN}/bin:${TOOLCHAIN}/usr/bin:${TOOLCHAIN}/usr/local/bin:${TOOLCHAIN}/opt/bin:${TOOLCHAIN}/opt/zephyr-sdk/arm-zephyr-eabi/bin:${HOME}/go/bin:/usr/local/bin:/usr/bin:/bin"
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

# XIAO BLE: MCUboot dual-slot, flashed via J-Link (replaces UF2 bootloader)
if [[ "$BOARD" == "xiao_ble" ]]; then
    EXTRA_ARGS="$EXTRA_ARGS -Dmcuboot_DTC_OVERLAY_FILE=${APP_DIR}/sysbuild/mcuboot_xiao_ble/partitions.overlay"
    EXTRA_ARGS="$EXTRA_ARGS -Dmcuboot_OVERLAY_CONFIG=${APP_DIR}/sysbuild/mcuboot_xiao_ble/mcuboot.conf"
    EXTRA_ARGS="$EXTRA_ARGS -DPM_STATIC_YML_FILE=${APP_DIR}/pm_static_xiao_ble.yml"
    EXTRA_ARGS="$EXTRA_ARGS -Dfirmware_PM_STATIC_YML_FILE=${APP_DIR}/pm_static_xiao_ble.yml"
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

echo "Flash image: ${BUILD_DIR}/merged.hex"
echo "OTA image:   ${BUILD_DIR}/firmware/zephyr/zephyr.signed.bin"

# Flash via J-Link (writes merged.hex including MCUboot)
if [ "$DO_FLASH_JLINK" = true ]; then
    echo ""
    echo "[build.sh] Flashing ${BOARD} via J-Link..."
    unset PYTHONHOME PYTHONPATH
    if [[ "$BOARD" == "xiao_ble" ]]; then
        # XIAO BLE: flash via nRF52840 DK J-Link Debug Out (SWD)
        # Must use JLinkExe — nrfutil targets the DK's on-board chip, not Debug Out
        echo -e "loadfile ${BUILD_DIR}/merged.hex\nr\ng\nexit\n" | \
            JLinkExe -USB 1050222631 -Device nRF52840_xxAA -If SWD -Speed 4000 -autoconnect 1
    else
        nrfutil device program --firmware "${BUILD_DIR}/merged.hex"
    fi
    echo "=== Flash complete ==="
fi

# OTA flash via MCUmgr (uploads signed bin over Thread)
if [ "$DO_FLASH" = true ]; then
    echo ""
    unset PYTHONHOME PYTHONPATH
    OTA_IMAGE="${BUILD_DIR}/firmware/zephyr/zephyr.signed.bin"
    if [ ! -f "${OTA_IMAGE}" ]; then
        echo "ERROR: Signed image not found: ${OTA_IMAGE}"
        exit 1
    fi
    if [ -z "${OTA_TARGET:-}" ]; then
        echo "ERROR: OTA requires target IPv6 address."
        echo "Usage: OTA_TARGET=<ipv6> ./build.sh ${BOARD} --flash"
        echo "   or: ./tools/scripts/ota_update.sh <ipv6> ${OTA_IMAGE}"
        exit 1
    fi
    echo "[build.sh] OTA flashing ${BOARD} at [${OTA_TARGET}]..."
    bash "${PROJECT_ROOT}/tools/scripts/ota_update.sh" "${OTA_TARGET}" "${OTA_IMAGE}"
    echo "=== OTA flash complete ==="
fi
