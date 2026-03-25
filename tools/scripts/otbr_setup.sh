#!/usr/bin/env bash
# otbr_setup.sh — Start OTBR Docker container and join the capstone Thread network
#
# Usage:  bash tools/scripts/otbr_setup.sh
#
# Prerequisites:
#   1. Docker installed
#   2. IP forwarding enabled (run once):
#      curl -sSL https://raw.githubusercontent.com/openthread/ot-br-posix/refs/heads/main/etc/docker/border-router/setup-host | sh
#   3. nRF52840 USB dongle (RCP) on /dev/ttyACM0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
COMPOSE_DIR="${PROJECT_ROOT}/tools/otbr"

# Thread network parameters — must match firmware prj.conf
CHANNEL=15
PANID="0xabcd"
EXTPANID="1111111122222222"
NETWORKKEY="00112233445566778899aabbccddeeff"
NETWORKNAME="ot_zephyr"
PSKC="d45635f5d9e2e234463a60306999b523"

# ── helpers ──────────────────────────────────────────────────────────────────

die() { echo "ERROR: $*" >&2; exit 1; }
ctl() { docker exec otbr ot-ctl "$@" 2>&1; }

wait_for_state() {
    local tries=0 max=60
    echo -n "  Waiting for Thread to attach ."
    while [[ $tries -lt $max ]]; do
        state=$(ctl state 2>/dev/null | head -1 | tr -d '[:space:]') || true
        case "$state" in
            leader|router|child) echo " OK ($state)"; return 0 ;;
        esac
        echo -n "."
        sleep 1
        (( tries++ ))
    done
    echo " TIMEOUT (last state: $state)"
    return 1
}

# ── 1. Kill any legacy ot-daemon ─────────────────────────────────────────────

if pgrep -x ot-daemon >/dev/null 2>&1; then
    echo "Stopping legacy ot-daemon..."
    sudo pkill -x ot-daemon || true
    sleep 1
fi

# ── 2. Pre-flight checks ─────────────────────────────────────────────────────

command -v docker >/dev/null 2>&1 || die "Docker not found. Install: https://docs.docker.com/engine/install/"
[[ -e /dev/ttyACM0 ]] || die "/dev/ttyACM0 not found — is the RCP dongle plugged in?"

# ── 3. Stop existing OTBR container if running ───────────────────────────────

if docker ps -q -f name=otbr 2>/dev/null | grep -q .; then
    echo "Stopping existing OTBR container..."
    docker compose -f "${COMPOSE_DIR}/docker-compose.yml" down
    sleep 2
fi

# ── 4. Start OTBR container ──────────────────────────────────────────────────

echo "Starting OTBR Docker container..."
docker compose -f "${COMPOSE_DIR}/docker-compose.yml" up -d

# ── 5. Wait for wpan0 to appear ──────────────────────────────────────────────

echo -n "  Waiting for wpan0 interface ."
for i in $(seq 1 30); do
    ip link show wpan0 >/dev/null 2>&1 && { echo " OK"; break; }
    echo -n "."
    sleep 1
    [[ $i -eq 30 ]] && die "wpan0 never appeared — check: docker logs otbr"
done

# ── 6. Wait for otbr-agent to initialize ─────────────────────────────────────

echo "  Waiting for otbr-agent to initialize..."
sleep 3

# ── 7. Configure Thread dataset ──────────────────────────────────────────────

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

# ── 8. Start Thread ──────────────────────────────────────────────────────────

echo "Starting Thread..."
ctl ifconfig up  >/dev/null
ctl thread start >/dev/null

wait_for_state || die "Thread did not attach. Check: docker logs otbr"

# ── 9. Fix deprecated addresses ───────────────────────────────────────────────

# OTBR (like raw ot-daemon) assigns wpan0 addresses with preferred_lft=0
# (deprecated), preventing Linux from using them as source addresses for
# outgoing packets (ping6, CoAP). Fix by setting preferred_lft forever.
sleep 2
echo "Fixing preferred lifetime on wpan0 addresses..."
while IFS= read -r addr_cidr; do
    echo "  Fixing $addr_cidr"
    sudo ip -6 addr change "$addr_cidr" dev wpan0 preferred_lft forever valid_lft forever 2>/dev/null || true
done < <(ip -6 addr show dev wpan0 scope global | awk '/inet6 /{print $2}')

# ── 10. Join realm-local multicast (ff03::1) so CoAP POSTs reach this host ───

echo "Joining ff03::1 multicast group on wpan0..."
sudo ip -6 maddr add ff03::1 dev wpan0 2>/dev/null || true

# ── 11. Summary ───────────────────────────────────────────────────────────────

echo ""
echo "=== OTBR ready ==="
echo ""
echo "wpan0 addresses:"
ip -6 addr show dev wpan0 | grep inet6 | awk '{print "  "$2}'
echo ""
echo "Thread state : $(ctl state 2>/dev/null | head -1)"
echo "OTBR web UI  : http://localhost:8080"
echo "Container log: docker logs otbr"
echo ""
echo "Find device addresses:"
echo "  docker exec otbr ot-ctl neighbor table"
echo "  ping6 -c3 -I wpan0 ff03::1"
echo ""
echo "Remote UCI commands (replace <addr> with device mesh address):"
echo "  python3 tools/scripts/uwb_tool.py coap://[<addr>] info"
echo "  python3 tools/scripts/uwb_tool.py coap://[<addr>] status"
echo ""
echo "To monitor live distance data from the anchor:"
echo "  python3 tools/monitor.py"
echo ""
echo "To stop OTBR:"
echo "  docker compose -f tools/otbr/docker-compose.yml down"
