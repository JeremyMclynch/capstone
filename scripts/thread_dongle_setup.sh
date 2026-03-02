#!/usr/bin/env bash
# thread_dongle_setup.sh — Start ot-daemon and join the capstone Thread network
#
# Usage:  ./scripts/thread_dongle_setup.sh
# Requires: sudo (for ot-daemon, ip commands)

set -euo pipefail

OT_DIR="/home/jeremy/Projects/openthread/src/openthread"
OT_DAEMON="$OT_DIR/build/posix/src/posix/ot-daemon"
OT_CTL="$OT_DIR/build/posix/src/posix/ot-ctl"
SPINEL_URI="spinel+hdlc+uart:///dev/ttyACM0?uart-baudrate=460800"
OT_LOG="/tmp/ot-daemon.log"

# Thread network parameters — must match prj.conf / firmware dataset
CHANNEL=15
PANID="0xabcd"
EXTPANID="1111111122222222"
NETWORKKEY="00112233445566778899aabbccddeeff"
NETWORKNAME="ot_zephyr"
MESHLOCALPREFIX="fdde:ad00:beef::/64"
PSKC="d45635f5d9e2e234463a60306999b523"

# ── helpers ──────────────────────────────────────────────────────────────────

die() { echo "ERROR: $*" >&2; exit 1; }
ctl() { sudo "$OT_CTL" "$@" 2>&1; }

wait_for_state() {
    local want="$1" tries=0 max=30
    echo -n "  Waiting for Thread state '$want' ."
    while [[ $tries -lt $max ]]; do
        state=$(ctl state 2>/dev/null | head -1 | tr -d '[:space:]') || true
        [[ "$state" == "$want" ]] && { echo " OK ($state)"; return 0; }
        echo -n "."
        sleep 1
        (( tries++ ))
    done
    echo " TIMEOUT (last state: $state)"
    return 1
}

# ── 1. Kill any existing ot-daemon ───────────────────────────────────────────

if pgrep -x ot-daemon >/dev/null 2>&1; then
    echo "Stopping existing ot-daemon..."
    sudo pkill -x ot-daemon || true
    sleep 1
fi

# ── 2. Start ot-daemon ───────────────────────────────────────────────────────

echo "Starting ot-daemon..."
[[ -x "$OT_DAEMON" ]] || die "ot-daemon not found at $OT_DAEMON"
sudo "$OT_DAEMON" -v "$SPINEL_URI" >"$OT_LOG" 2>&1 &
DAEMON_PID=$!
echo "  PID $DAEMON_PID — log: $OT_LOG"

# Wait for wpan0 to appear
echo -n "  Waiting for wpan0 interface ."
for i in $(seq 1 20); do
    ip link show wpan0 >/dev/null 2>&1 && { echo " OK"; break; }
    echo -n "."
    sleep 1
    [[ $i -eq 20 ]] && die "wpan0 never appeared — check $OT_LOG"
done

# ── 3. Configure Thread dataset ──────────────────────────────────────────────

echo "Configuring Thread dataset..."
ctl dataset init new            >/dev/null
ctl dataset channel "$CHANNEL"  >/dev/null
ctl dataset panid   "$PANID"    >/dev/null
ctl dataset extpanid "$EXTPANID" >/dev/null
ctl dataset networkkey "$NETWORKKEY" >/dev/null
ctl dataset networkname "$NETWORKNAME" >/dev/null
ctl dataset meshlocalprefix "$MESHLOCALPREFIX" >/dev/null
ctl dataset pskc "$PSKC"        >/dev/null
ctl dataset commit active       >/dev/null
echo "  Dataset committed."

# ── 4. Start Thread ──────────────────────────────────────────────────────────

echo "Starting Thread..."
ctl ifconfig up  >/dev/null
ctl thread start >/dev/null

wait_for_state "router" || {
    # Router role not guaranteed; check for any attached state
    state=$(ctl state 2>/dev/null | head -1 | tr -d '[:space:]') || true
    case "$state" in
        leader|child|router) echo "  Joined as $state — OK" ;;
        *) die "Thread did not join (state=$state). Check $OT_LOG" ;;
    esac
}

# ── 5. Bring up wpan0 and fix deprecated address ─────────────────────────────

echo "Bringing up wpan0..."
sudo ip link set wpan0 up

# Give OT a moment to register its mesh-local EID on wpan0
sleep 2

# ot-daemon assigns addresses with preferred_lft=0 (deprecated), which prevents
# Linux from using them as source addresses for outgoing packets (ping6, CoAP).
# Fix by re-adding each global-scope address with preferred_lft forever.
echo "Fixing preferred lifetime on mesh-local addresses..."
mapfile -t ADDRS < <(ip -6 addr show dev wpan0 scope global | awk '/inet6 /{print $2}')
for ADDR in "${ADDRS[@]}"; do
    echo "  Fixing $ADDR"
    sudo ip -6 addr change "$ADDR" dev wpan0 preferred_lft forever valid_lft forever 2>/dev/null || true
done

# ── 6. Join realm-local multicast (ff03::1) so CoAP POSTs reach this host ────

echo "Joining ff03::1 multicast group on wpan0..."
# ff03::1 = Thread realm-local all-nodes multicast — anchor sends CoAP here
sudo ip -6 maddr add ff03::1 dev wpan0 2>/dev/null || true

# ── 7. Summary ────────────────────────────────────────────────────────────────

echo ""
echo "=== Thread dongle ready ==="
echo ""
echo "wpan0 addresses:"
ip -6 addr show dev wpan0 | grep inet6 | awk '{print "  "$2}'
echo ""
echo "Thread state : $(ctl state 2>/dev/null | head -1)"
echo "ot-daemon log: $OT_LOG"
echo ""
echo "Find device addresses:"
echo "  sudo ot-ctl neighbor table"
echo "  ping6 -c3 -I wpan0 ff03::1"
echo ""
echo "Remote UCI commands (replace <addr> with device mesh address):"
echo "  python3 scripts/uwb_tool.py coap://[<addr>] info"
echo "  python3 scripts/uwb_tool.py coap://[<addr>] status"
echo ""
echo "To monitor live distance data from the anchor:"
echo "  python3 monitor.py"
echo ""
echo "To run the full server (CoAP + SQLite):"
echo "  cd server && python3 main.py"
