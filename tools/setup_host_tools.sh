#!/usr/bin/env bash
# setup_host_tools.sh — Build/install host-side tools for the UWB project
#
# Builds OpenThread POSIX tools (ot-daemon, ot-ctl) and installs mcumgr CLI.
#
# Usage:  bash tools/setup_host_tools.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OT_BUILD_DIR="${SCRIPT_DIR}/openthread"

echo "=== Host Tool Setup ==="

# ── 1. OpenThread POSIX (ot-daemon + ot-ctl) ──────────────────────────────────

echo ""
echo "--- OpenThread POSIX tools ---"
if [[ -x "${OT_BUILD_DIR}/ot-daemon" && -x "${OT_BUILD_DIR}/ot-ctl" ]]; then
    echo "Already built at ${OT_BUILD_DIR}/ — skipping."
    echo "  Delete ${OT_BUILD_DIR}/ to force rebuild."
else
    echo "Cloning and building OpenThread (this may take a few minutes)..."
    OT_SRC=$(mktemp -d)
    git clone --depth 1 https://github.com/openthread/openthread.git "$OT_SRC"
    cd "$OT_SRC"
    ./script/bootstrap   # install system build deps (cmake, ninja, etc.)
    ./script/cmake-build posix
    mkdir -p "$OT_BUILD_DIR"
    cp build/posix/src/posix/ot-daemon "$OT_BUILD_DIR/"
    cp build/posix/src/posix/ot-ctl "$OT_BUILD_DIR/"
    cd "$PROJECT_ROOT"
    rm -rf "$OT_SRC"
    echo "Built: ${OT_BUILD_DIR}/ot-daemon, ot-ctl"
fi

# ── 2. mcumgr CLI (for OTA firmware updates) ──────────────────────────────────

echo ""
echo "--- mcumgr CLI ---"
if command -v mcumgr >/dev/null 2>&1; then
    echo "Already installed: $(command -v mcumgr)"
else
    if command -v go >/dev/null 2>&1; then
        echo "Installing mcumgr via Go..."
        go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
        echo "Installed to $(go env GOPATH)/bin/mcumgr"
        echo "Ensure $(go env GOPATH)/bin is in your PATH."
    else
        echo "Go not found. Install Go first, then run:"
        echo "  go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest"
    fi
fi

# ── 3. Python dependencies ────────────────────────────────────────────────────

echo ""
echo "--- Python dependencies ---"
echo "Install with:  pip install aiocoap pyserial"

echo ""
echo "=== Done ==="
