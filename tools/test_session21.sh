#!/bin/bash
set -e
cd "$(dirname "$0")/.."

python3 tools/testnet_server.py > /tmp/testnet.log 2>&1 &
TESTNET_PID=$!
sleep 1

cleanup() {
    kill -9 $TESTNET_PID 2>/dev/null || true
    cat /tmp/testnet.log 2>/dev/null || true
}
trap cleanup EXIT INT TERM

LOGFILE=$(mktemp)

(
    sleep 3
    printf "ifconfig eth0 10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2\n"
    sleep 0.5
    printf "ifconfig\n"
    sleep 0.5
    printf "ping -c 2 10.0.2.2\n"
    sleep 2
    printf "nettest\n"
    sleep 35
    printf "cat /proc/sys/net/killswitch\n"
    sleep 0.5
    printf "netstat -a\n"
    sleep 0.5
    printf "exit\n"
) | timeout 70s qemu-system-i386 -vga std -m 256 -device virtio-net-pci,netdev=n0 -netdev user,id=n0 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE" 2>&1 || true

tr -d '\r' < "$LOGFILE" > "${LOGFILE}.tmp" && mv "${LOGFILE}.tmp" "$LOGFILE"
cat "$LOGFILE"

grep -q "ALL PHASE 6 NETWORK TESTS PASSED!" "$LOGFILE" || { echo "FAIL: nettest failed"; rm -f "$LOGFILE"; exit 1; }
echo "[PASS] Session 21 Part B verified!"
rm -f "$LOGFILE"

