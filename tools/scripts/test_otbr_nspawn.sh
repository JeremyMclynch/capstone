#!/usr/bin/env bash
# test_otbr_nspawn.sh — Verify OTBR Docker setup on a fresh Arch Linux system
#                       inside an ephemeral systemd-nspawn container.
#
# Creates an Arch rootfs, boots it with systemd-nspawn, installs Docker,
# runs the full OTBR setup, verifies Thread network join, then cleans up.
#
# Requirements:
#   - Run as root (sudo)
#   - systemd-nspawn + machinectl
#   - Docker (on host, to bootstrap Arch rootfs)
#   - nRF52840 USB dongle (RCP) on /dev/ttyACM0
#   - No existing 'otbr' container or wpan0 interface
#
# Usage:  sudo bash tools/scripts/test_otbr_nspawn.sh

set -euo pipefail

MACHINE_NAME="test-otbr"
CONTAINER_DIR="/var/lib/machines/${MACHINE_NAME}"
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOOT_TIMEOUT=60
TEST_TIMEOUT=600  # 10 minutes for full test (image pull can be slow)

# ── Helpers ─────────────────────────────────────────────────────────────────

die() { echo "ERROR: $*" >&2; exit 1; }

cleanup() {
    echo ""
    echo "=== Cleanup ==="

    # Terminate nspawn machine
    if machinectl show "$MACHINE_NAME" &>/dev/null; then
        echo "Terminating nspawn container..."
        machinectl terminate "$MACHINE_NAME" 2>/dev/null || true
        sleep 2
        machinectl kill "$MACHINE_NAME" 2>/dev/null || true
        sleep 1
    fi

    # Remove rootfs
    if [[ -d "$CONTAINER_DIR" ]]; then
        echo "Removing container rootfs..."
        rm -rf "$CONTAINER_DIR"
    fi

    echo "Cleanup done."
}
trap cleanup EXIT

# ── Pre-flight checks ──────────────────────────────────────────────────────

echo "=== OTBR nspawn Integration Test ==="
echo ""

[[ $EUID -eq 0 ]] || die "Must run as root (sudo)"
command -v systemd-nspawn >/dev/null 2>&1 || die "systemd-nspawn not found (install systemd-container)"
command -v machinectl >/dev/null 2>&1 || die "machinectl not found"
command -v docker >/dev/null 2>&1 || die "Docker not found (needed to bootstrap Arch rootfs)"
[[ -e /dev/ttyACM0 ]] || die "/dev/ttyACM0 not found — is the RCP dongle plugged in?"

if docker ps -q -f name=otbr 2>/dev/null | grep -q .; then
    die "An 'otbr' Docker container is already running. Stop it first: docker compose -f tools/otbr/docker-compose.yml down"
fi

if ip link show wpan0 &>/dev/null; then
    die "wpan0 interface already exists. Stop any running OTBR first."
fi

# Detect default route interface
INFRA_IF=$(ip route show default | awk '{print $5; exit}')
[[ -n "$INFRA_IF" ]] || die "Cannot detect default route interface for OT_INFRA_IF"
echo "Detected infrastructure interface: $INFRA_IF"

# ── Phase 1: Bootstrap Arch Linux rootfs ────────────────────────────────────

echo ""
echo "=== Phase 1: Bootstrap Arch Linux rootfs ==="

if [[ -d "$CONTAINER_DIR" ]]; then
    echo "Removing stale rootfs at $CONTAINER_DIR..."
    rm -rf "$CONTAINER_DIR"
fi
mkdir -p "$CONTAINER_DIR"

echo "Creating Arch Linux rootfs via Docker export..."
CID=$(docker create archlinux:latest /bin/true)
docker export "$CID" | tar -xf - -C "$CONTAINER_DIR"
docker rm "$CID" >/dev/null
echo "  Rootfs created at $CONTAINER_DIR"

# Generate machine ID
systemd-machine-id-setup --root="$CONTAINER_DIR"

# DNS resolution
cp /etc/resolv.conf "$CONTAINER_DIR/etc/resolv.conf"

# Mask services that can hang boot
ln -sf /dev/null "$CONTAINER_DIR/etc/systemd/system/systemd-networkd-wait-online.service"
ln -sf /dev/null "$CONTAINER_DIR/etc/systemd/system/systemd-time-wait-sync.service"

# Copy inner test script
cp "$PROJECT_ROOT/tools/scripts/_otbr_nspawn_inner.sh" "$CONTAINER_DIR/run-test.sh"
chmod +x "$CONTAINER_DIR/run-test.sh"

# Write detected infra interface for inner script (use /etc, not /tmp — nspawn mounts fresh tmpfs on /tmp)
echo "$INFRA_IF" > "$CONTAINER_DIR/etc/infra_if"

echo "  Rootfs prepared."

# Enable IP forwarding on host (shared network namespace — /proc/sys is read-only inside nspawn)
echo "Enabling IP forwarding on host (shared with container)..."
sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# ── Phase 2: Launch nspawn container ────────────────────────────────────────

echo ""
echo "=== Phase 2: Launch nspawn container ==="

systemd-nspawn \
    --directory="$CONTAINER_DIR" \
    --machine="$MACHINE_NAME" \
    --boot \
    --capability=all \
    --system-call-filter= \
    --private-users=no \
    --bind=/dev/ttyACM0:/dev/ttyACM0 \
    --bind=/dev/net/tun:/dev/net/tun \
    --bind-ro="$PROJECT_ROOT":/capstone \
    --property=DeviceAllow='char-ttyACM rwm' \
    --property=DeviceAllow='char-tun rwm' \
    --property=Delegate=yes \
    --tmpfs=/var/lib/docker:exec \
    &

NSPAWN_PID=$!
echo "  nspawn started (PID $NSPAWN_PID)"

# ── Phase 3: Wait for boot ─────────────────────────────────────────────────

echo -n "  Waiting for container to boot ."
for i in $(seq 1 "$BOOT_TIMEOUT"); do
    if machinectl show "$MACHINE_NAME" --property=State 2>/dev/null | grep -q "running"; then
        echo " OK (${i}s)"
        break
    fi
    echo -n "."
    sleep 1
    if [[ $i -eq $BOOT_TIMEOUT ]]; then
        echo " TIMEOUT"
        die "Container failed to boot within ${BOOT_TIMEOUT}s"
    fi
done

# Give systemd a moment to reach multi-user.target
sleep 5

# ── Phase 4: Run inner test ────────────────────────────────────────────────

echo ""
echo "=== Phase 3: Running OTBR test inside container ==="
echo ""

# systemd-run --wait --pipe propagates exit code and streams output
set +e
systemd-run \
    --machine="$MACHINE_NAME" \
    --wait \
    --pipe \
    --collect \
    --service-type=exec \
    --property=Delegate=yes \
    --setenv=TERM=xterm \
    /bin/bash /run-test.sh
TEST_EXIT=$?
set -e

# ── Phase 5: Report ────────────────────────────────────────────────────────

echo ""
if [[ $TEST_EXIT -eq 0 ]]; then
    echo "=== OTBR nspawn test PASSED ==="
else
    echo "=== OTBR nspawn test FAILED (exit code $TEST_EXIT) ==="
    echo ""
    echo "Debug: check container logs with"
    echo "  machinectl shell $MACHINE_NAME /bin/bash"
    echo "  docker logs otbr"
fi

exit $TEST_EXIT
# cleanup runs via trap
