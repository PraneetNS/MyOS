#!/bin/bash
set -e
cd "$(dirname "$0")/.."

LOGFILE20=$(mktemp)
LOGFILE20_LOW=$(mktemp)

# Part A: -m 64 boot still works; memd and indexd register
(
    sleep 3
    printf "shinfo\n"
    sleep 0.5
    printf "cat /proc/meminfo\n"
    sleep 0.5
    printf "agentctl create pagent --principal alice --mem-read priv --mem-write priv --bus-call mem.\n"
    sleep 0.5
    printf "agentctl status 1\n"
    sleep 0.5
    printf "cat /proc/agents/1/caps\n"
    sleep 0.5
    printf "exit\n"
) | timeout 30s qemu-system-x86_64 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE20_LOW" 2>&1 || true
tr -d '\r' < "$LOGFILE20_LOW" > "${LOGFILE20_LOW}.tmp" && mv "${LOGFILE20_LOW}.tmp" "$LOGFILE20_LOW"
grep -q "reg mem.put" "$LOGFILE20_LOW" || { echo "FAIL: memd did not register in -m 64 boot"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "reg fs.search" "$LOGFILE20_LOW" || { echo "FAIL: indexd did not register in -m 64 boot"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "MemTotal:       65020 kB" "$LOGFILE20_LOW" || { echo "FAIL: -m 64 MemTotal unexpected"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "Principal:.*alice" "$LOGFILE20_LOW" || { echo "FAIL: agentctl --principal / status principal missing"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "MemRead:	priv" "$LOGFILE20_LOW" || { echo "FAIL: MemRead cap missing from /proc/agents/1/caps"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
grep -q "MemWrite:	priv" "$LOGFILE20_LOW" || { echo "FAIL: MemWrite cap missing from /proc/agents/1/caps"; rm -f "$LOGFILE20" "$LOGFILE20_LOW"; exit 1; }
echo "[PASS] -m 64 boot with memd/indexd verified!"
rm -f "$LOGFILE20_LOW"

# Part B: full memory + index test suite
(
    sleep 3
    printf "memdtest\n"
    sleep 40
    printf "memctl list alice\n"
    sleep 2
    printf "sync\n"
    sleep 2
    printf "exit\n"
) | timeout 120s qemu-system-x86_64 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE20" 2>&1 || true
tr -d '\r' < "$LOGFILE20" > "${LOGFILE20}.tmp" && mv "${LOGFILE20}.tmp" "$LOGFILE20"
cat "$LOGFILE20"

echo "[TEST] Asserting Session 20 results..."
grep -q "ALL MEMORY TESTS PASSED!" "$LOGFILE20" || { echo "FAIL: memdtest suite failed"; rm -f "$LOGFILE20"; exit 1; }
grep -q "FAILED:" "$LOGFILE20" && { echo "FAIL: memdtest reported a failure"; rm -f "$LOGFILE20"; exit 1; }
for t in "Stable principals" "Kernel-vouched principal" "memd put/search" "CAP_MEM_READ/WRITE namespace" "Provenance:" "Crash safety" "mem.forget physically" "indexd: fs.search filtered" "Audit chain records"; do
    grep -q "PASSED: $t" "$LOGFILE20" || { echo "FAIL: missing '$t'"; rm -f "$LOGFILE20"; exit 1; }
done

if strings disk.img | grep -q "ROTATION123"; then echo "FAIL: deleted memory text still present on raw disk image"; rm -f "$LOGFILE20"; exit 1; fi
strings disk.img | grep -q "LIVEMARKER_KEEP_7788" || { echo "FAIL: control record not found on disk (grep not meaningful)"; rm -f "$LOGFILE20"; exit 1; }

echo "[PASS] Session 20: Persistent memory, principals, memd, memctl, and indexd verified!"
rm -f "$LOGFILE20"

