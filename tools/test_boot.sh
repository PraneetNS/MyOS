#!/bin/bash
set -e
cd "$(dirname "$0")/.."

LOGFILE=$(mktemp)
trap 'rm -f "$LOGFILE"' EXIT

echo "[TEST] Running headless boot and command test..."

# Feed commands with delays to allow boot and execution
(
    sleep 2
    printf "help\n"
    sleep 1
    printf "argtest hello world 123\n"
    sleep 1
    printf "exit\n"
) | timeout 15s qemu-system-i386 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE" 2>&1 || true

cat "$LOGFILE"

echo "[TEST] Asserting test output..."

# Assert BOOT OK
if ! grep -q "BOOT OK" "$LOGFILE"; then
    echo "FAIL: 'BOOT OK' not found in boot log"
    exit 1
fi
echo "[PASS] 'BOOT OK' verified"

# Assert shell prompt / welcome
if ! grep -q "MyOS Userland Shell (sh)" "$LOGFILE"; then
    echo "FAIL: Shell welcome message not found"
    exit 1
fi
echo "[PASS] Shell welcome message verified"

# Assert argtest output
if ! grep -q "argtest running" "$LOGFILE"; then
    echo "FAIL: 'argtest running' not found"
    exit 1
fi
if ! grep -q "argc = 4" "$LOGFILE"; then
    echo "FAIL: 'argc = 4' not found"
    exit 1
fi
if ! grep -q "argv\[0\] = 'argtest'" "$LOGFILE"; then
    echo "FAIL: 'argv[0] = 'argtest'' not found"
    exit 1
fi
if ! grep -q "argv\[1\] = 'hello'" "$LOGFILE"; then
    echo "FAIL: 'argv[1] = 'hello'' not found"
    exit 1
fi
if ! grep -q "argv\[2\] = 'world'" "$LOGFILE"; then
    echo "FAIL: 'argv[2] = 'world'' not found"
    exit 1
fi
if ! grep -q "argv\[3\] = '123'" "$LOGFILE"; then
    echo "FAIL: 'argv[3] = '123'' not found"
    exit 1
fi
if ! grep -q "argtest done" "$LOGFILE"; then
    echo "FAIL: 'argtest done' not found"
    exit 1
fi
echo "[PASS] argtest output and arguments verified"

echo "ALL TESTS PASSED!"
exit 0
