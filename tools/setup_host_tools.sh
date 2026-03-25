#!/usr/bin/env bash
# setup_host_tools.sh — Build/install host-side tools for the UWB project
#
# Builds OpenThread POSIX tools (ot-daemon, ot-ctl) and installs mcumgr CLI.
#
# Usage:  bash tools/setup_host_tools.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== Host Tool Setup ==="

# ── 1. Docker + OTBR image (replaces ot-daemon/ot-ctl build) ──────────────────

echo ""
echo "--- OTBR Docker image ---"
if command -v docker >/dev/null 2>&1; then
    echo "Docker found: $(docker --version)"
    echo "Pulling OpenThread Border Router image..."
    docker pull openthread/border-router:latest
    echo ""
    echo "Enable IP forwarding (run once, if not already done):"
    echo "  curl -sSL https://raw.githubusercontent.com/openthread/ot-br-posix/refs/heads/main/etc/docker/border-router/setup-host | sh"
else
    echo "Docker not found. Install Docker Engine first:"
    echo "  https://docs.docker.com/engine/install/"
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
