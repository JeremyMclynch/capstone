#!/usr/bin/env bash
# _otbr_nspawn_inner.sh — Runs inside the nspawn container to test OTBR setup
#
# This script is NOT meant to be run directly. It is invoked by test_otbr_nspawn.sh.
set -euo pipefail

INFRA_IF=$(cat /etc/infra_if 2>/dev/null || echo "eth0")

echo "=== OTBR Fresh Install Test (Arch Linux) ==="
echo "Infrastructure interface: $INFRA_IF"
echo ""

# ── 1. Initialize pacman keyring ────────────────────────────────────────────
echo "Initializing pacman keyring..."
pacman-key --init
pacman-key --populate archlinux

# ── 2. Install Docker + dependencies ────────────────────────────────────────
echo "Installing Docker and dependencies..."
pacman -Sy --noconfirm docker docker-compose curl iproute2 python python-pip

# ── 3. Start Docker daemon ──────────────────────────────────────────────────
# Configure Docker for nspawn environment:
# - use vfs storage driver (overlay-on-overlay may fail in nspawn tmpfs)
mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<DJEOF
{
    "storage-driver": "vfs"
}
DJEOF

# Wrap runc to inject --no-new-keyring flag.
# Inside nspawn, the kernel blocks session keyring creation (KEYCTL_JOIN_SESSION_KEYRING).
# runc v1.2+ supports --no-new-keyring to skip this. We wrap the real binary.
RUNC_PATH=$(command -v runc)
mv "$RUNC_PATH" "${RUNC_PATH}.real"
cat > "$RUNC_PATH" <<'WEOF'
#!/bin/bash
# Wrapper: inject --no-new-keyring after create/run subcommands
# runc create [opts] <id> → runc.real create --no-new-keyring [opts] <id>
newargs=()
injected=false
for arg in "$@"; do
    newargs+=("$arg")
    if [[ "$injected" == false ]] && [[ "$arg" == "create" || "$arg" == "run" ]]; then
        newargs+=("--no-new-keyring")
        injected=true
    fi
done
exec /usr/bin/runc.real "${newargs[@]}"
WEOF
chmod +x "$RUNC_PATH"

echo "Starting Docker daemon..."
systemctl start docker
if ! systemctl is-active --quiet docker; then
    echo "FATAL: Docker daemon failed to start"
    journalctl -u docker --no-pager -n 30
    exit 1
fi
echo "Docker running: $(docker --version)"

# ── 4. Pull OTBR image ─────────────────────────────────────────────────────
echo "Pulling OTBR Docker image..."
docker pull openthread/border-router:latest

# ── 5. IP forwarding ───────────────────────────────────────────────────────
# IP forwarding is set on the host (shared network namespace, /proc/sys is read-only in nspawn)
echo "IP forwarding: set by host (shared network namespace)"

# ── 6. Create patched OTBR config ──────────────────────────────────────────
WORK_DIR=/tmp/otbr-work
mkdir -p "$WORK_DIR"

# Patched compose: add security_opt to work around session keyring restriction in nspawn
cat > "$WORK_DIR/docker-compose.yml" <<DCEOF
services:
  otbr:
    image: openthread/border-router:latest
    container_name: otbr
    network_mode: host
    cap_add:
      - NET_ADMIN
      - SYS_ADMIN
    security_opt:
      - seccomp:unconfined
      - no-new-privileges:false
    devices:
      - /dev/ttyACM0:/dev/ttyACM0
      - /dev/net/tun:/dev/net/tun
    volumes:
      - otbr-data:/data
    env_file:
      - otbr-env.list
    restart: "no"

volumes:
  otbr-data:
DCEOF

cat > "$WORK_DIR/otbr-env.list" <<EOF
OT_RCP_DEVICE=spinel+hdlc+uart:///dev/ttyACM0?uart-baudrate=460800
OT_INFRA_IF=${INFRA_IF}
OT_THREAD_IF=wpan0
OT_LOG_LEVEL=7
EOF

# ── 7. Start OTBR container ────────────────────────────────────────────────
echo "Starting OTBR container..."
docker compose -f "$WORK_DIR/docker-compose.yml" up -d

# ── 8. Wait for wpan0 ──────────────────────────────────────────────────────
echo -n "Waiting for wpan0 interface ."
for i in $(seq 1 30); do
    if ip link show wpan0 >/dev/null 2>&1; then
        echo " OK"
        break
    fi
    echo -n "."
    sleep 1
    if [[ $i -eq 30 ]]; then
        echo " TIMEOUT"
        echo "FATAL: wpan0 never appeared"
        docker logs otbr 2>&1 | tail -30
        exit 1
    fi
done

# ── 9. Wait for otbr-agent to initialize ───────────────────────────────────
echo "Waiting for otbr-agent to initialize..."
sleep 3

# ── 10. Configure Thread dataset ───────────────────────────────────────────
CHANNEL=15
PANID="0xabcd"
EXTPANID="1111111122222222"
NETWORKKEY="00112233445566778899aabbccddeeff"
NETWORKNAME="ot_zephyr"
PSKC="d45635f5d9e2e234463a60306999b523"

ctl() { docker exec otbr ot-ctl "$@" 2>&1; }

echo "Configuring Thread dataset..."
ctl dataset clear                        >/dev/null
ctl dataset channel "$CHANNEL"           >/dev/null
ctl dataset panid   "$PANID"             >/dev/null
ctl dataset extpanid "$EXTPANID"         >/dev/null
ctl dataset networkkey "$NETWORKKEY"     >/dev/null
ctl dataset networkname "$NETWORKNAME"   >/dev/null
ctl dataset pskc "$PSKC"                >/dev/null
ctl dataset activetimestamp 1            >/dev/null
ctl dataset securitypolicy 672 onrc 0   >/dev/null
ctl dataset commit active                >/dev/null
echo "  Dataset committed."

# ── 11. Start Thread ───────────────────────────────────────────────────────
echo "Starting Thread..."
ctl ifconfig up  >/dev/null
ctl thread start >/dev/null
# Speed up router promotion (default jitter can be up to 120s)
ctl routerselectionjitter 1 >/dev/null 2>&1 || true

# ── 12. Wait for Thread to attach ──────────────────────────────────────────
STATE=""
echo -n "Waiting for Thread to attach ."
for i in $(seq 1 60); do
    STATE=$(ctl state 2>/dev/null | head -1 | tr -d '[:space:]') || true
    case "$STATE" in
        leader|router|child) echo " OK ($STATE)"; break ;;
    esac
    echo -n "."
    sleep 1
    if [[ $i -eq 60 ]]; then
        echo " TIMEOUT (last state: $STATE)"
    fi
done

# ── 13. Fix deprecated IPv6 addresses ──────────────────────────────────────
sleep 2
echo "Fixing preferred lifetime on wpan0 addresses..."
while IFS= read -r addr_cidr; do
    echo "  Fixing $addr_cidr"
    ip -6 addr change "$addr_cidr" dev wpan0 preferred_lft forever valid_lft forever 2>/dev/null || true
done < <(ip -6 addr show dev wpan0 scope global 2>/dev/null | awk '/inet6 /{print $2}')

# ── 14. Join realm-local multicast ─────────────────────────────────────────
echo "Joining ff03::1 multicast group on wpan0..."
ip -6 maddr add ff03::1 dev wpan0 2>/dev/null || true

# ── 15. Install Python dependencies and test monitor.py ─────────────────────
echo "Setting up Python venv..."
python -m venv /tmp/venv
source /tmp/venv/bin/activate
pip install --quiet aiocoap

# Wait for OTBR to become leader/router (may need to take over from previous leader)
echo -n "Waiting for Thread role promotion ."
for i in $(seq 1 60); do
    STATE=$(ctl state 2>/dev/null | head -1 | tr -d '[:space:]') || true
    case "$STATE" in
        leader|router) echo " OK ($STATE after ${i}s)"; break ;;
    esac
    echo -n "."
    sleep 1
    if [[ $i -eq 60 ]]; then
        echo " still $STATE after 60s (continuing anyway)"
    fi
done

# Re-fix deprecated addresses (OTBR adds new ones after role change)
echo "Re-fixing preferred lifetime on wpan0 addresses..."
while IFS= read -r addr_cidr; do
    ip -6 addr change "$addr_cidr" dev wpan0 preferred_lft forever valid_lft forever 2>/dev/null || true
done < <(ip -6 addr show dev wpan0 scope global 2>/dev/null | awk '/inet6 /{print $2}')

# Test CoAP reception via raw UDP socket with multicast join on wpan0
echo "Testing CoAP reception on port 5683 (10s)..."
COAP_LOG=/tmp/coap_reception.log
/tmp/venv/bin/python -u -c "
import socket, struct, time
sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('::', 5683))
# Join ff03::1 multicast on wpan0
wpan_idx = socket.if_nametoindex('wpan0')
mreq = struct.pack('16sI', socket.inet_pton(socket.AF_INET6, 'ff03::1'), wpan_idx)
sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)
sock.settimeout(1.0)
dist_count = 0
evt_count = 0
total = 0
end = time.time() + 10
while time.time() < end:
    try:
        data, addr = sock.recvfrom(1024)
        total += 1
        # Find CoAP payload after 0xFF marker
        marker = data.find(b'\xff')
        if marker >= 0:
            payload = data[marker+1:]
            if len(payload) == 20 or len(payload) == 12:
                dist_count += 1
            elif len(payload) == 6:
                evt_count += 1
    except socket.timeout:
        pass
print(f'Total: {total} CoAP packets ({dist_count} distance, {evt_count} events) in 10s')
sock.close()
" > "$COAP_LOG" 2>&1
echo "  Reception results:"
cat "$COAP_LOG"
COAP_TOTAL=$(grep -oP 'Total: \K\d+' "$COAP_LOG" 2>/dev/null || echo "0")
COAP_DIST=$(grep -oP '\((\d+) distance' "$COAP_LOG" 2>/dev/null | grep -oP '\d+' || echo "0")

# ══════════════════════════════════════════════════════════════════════════════
# Verification
# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Verification ==="
PASS=0
FAIL=0

# Test 1: wpan0 exists
if ip link show wpan0 >/dev/null 2>&1; then
    echo "PASS: wpan0 interface exists"
    PASS=$((PASS + 1))
else
    echo "FAIL: wpan0 interface not found"
    FAIL=$((FAIL + 1))
fi

# Test 2: wpan0 has global IPv6 addresses
ADDRS=$(ip -6 addr show dev wpan0 scope global 2>/dev/null | grep -c inet6 || true)
if [[ "$ADDRS" -gt 0 ]]; then
    echo "PASS: wpan0 has $ADDRS global IPv6 address(es)"
    PASS=$((PASS + 1))
else
    echo "FAIL: wpan0 has no global IPv6 addresses"
    FAIL=$((FAIL + 1))
fi

# Test 3: Thread state is attached
case "$STATE" in
    leader|router|child)
        echo "PASS: Thread state is $STATE"
        PASS=$((PASS + 1))
        ;;
    *)
        echo "FAIL: Thread state is '$STATE' (expected leader/router/child)"
        FAIL=$((FAIL + 1))
        ;;
esac

# Test 4: OTBR container is running
if docker ps --filter name=otbr --format '{{.Status}}' | grep -qi up; then
    echo "PASS: OTBR container is running"
    PASS=$((PASS + 1))
else
    echo "FAIL: OTBR container is not running"
    FAIL=$((FAIL + 1))
fi

# Test 5: OTBR web UI responds
if curl -sf -o /dev/null http://localhost:8080; then
    echo "PASS: OTBR web UI responding on :8080"
    PASS=$((PASS + 1))
else
    echo "FAIL: OTBR web UI not responding"
    FAIL=$((FAIL + 1))
fi

# Test 6: CoAP packets received from devices
if [[ "$COAP_TOTAL" -gt 0 ]]; then
    echo "PASS: received $COAP_TOTAL CoAP packets ($COAP_DIST distance) from mesh devices"
    PASS=$((PASS + 1))
else
    echo "FAIL: no CoAP packets received from mesh devices in 10s"
    FAIL=$((FAIL + 1))
fi

# ── Teardown ────────────────────────────────────────────────────────────────
echo ""
echo "Tearing down OTBR container..."
docker compose -f "$WORK_DIR/docker-compose.yml" down 2>/dev/null || true
rm -rf "$WORK_DIR"

# ── Summary ─────────────────────────────────────────────────────────────────
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]]
