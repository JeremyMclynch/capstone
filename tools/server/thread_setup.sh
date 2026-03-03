#!/bin/bash
# ================================================================
# Thread Network Setup for ot-daemon (run as root / sudo)
#
# Configures the ot-daemon with the same credentials as the
# firmware and starts the Thread stack as Leader.
#
# Credentials must match firmware/prj.conf:
#   CONFIG_OPENTHREAD_CHANNEL=15
#   CONFIG_OPENTHREAD_PANID=43981 (0xABCD)
#   CONFIG_OPENTHREAD_NETWORKKEY="00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff"
#   CONFIG_OPENTHREAD_XPANID="11:11:11:11:22:22:22:22"
# ================================================================

OT_CTL=/home/jeremy/ncs/v3.2.2/modules/lib/openthread/build/posix/src/posix/ot-ctl

run() {
    echo ">> ot-ctl $*"
    $OT_CTL "$@"
    sleep 0.3
}

echo "=== Stopping any existing Thread session ==="
run thread stop 2>/dev/null || true
run ifconfig down 2>/dev/null || true
sleep 1

echo ""
echo "=== Configuring Thread dataset ==="
run dataset init new
run dataset channel 15
run dataset networkkey 00112233445566778899aabbccddeeff
run dataset panid 0xabcd
run dataset extpanid 1111111122222222
run dataset networkname UWBTracker
run dataset commit active

echo ""
echo "=== Starting Thread interface ==="
run ifconfig up
sleep 1
run thread start
sleep 3

echo ""
echo "=== Thread state ==="
run state
run ipaddr
run channel
run panid
run networkkey

echo ""
echo "=== wpan0 interface ==="
ip addr show wpan0

echo ""
echo "Setup complete. ot-daemon is now running as Thread Leader."
echo "Devices with matching credentials will attach automatically."
