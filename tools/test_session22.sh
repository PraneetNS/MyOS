#!/bin/bash
set -e
cd "$(dirname "$0")/.."

echo "=========================================================="
echo "[TEST] Session 22: x86-64 Long Mode, UEFI Boot, >4GB RAM, and CPU Hardening Suite"
echo "=========================================================="

LOGFILE_UEFI=$(mktemp)
LOGFILE_RAM6G=$(mktemp)
LOGFILE_RAM8G=$(mktemp)
LOGFILE_HARDEN=$(mktemp)

cleanup() {
    rm -f "$LOGFILE_UEFI" "$LOGFILE_RAM6G" "$LOGFILE_RAM8G" "$LOGFILE_HARDEN"
}
trap cleanup EXIT INT TERM

# -------------------------------------------------------------
# Part 1: UEFI Boot with OVMF Firmware
# -------------------------------------------------------------
echo "[TEST] Session 22 Part 1: Booting via UEFI (OVMF firmware)..."
OVMF_PATH="/usr/share/ovmf/OVMF.fd"
if [ ! -f "$OVMF_PATH" ]; then
    OVMF_PATH=$(find /usr/share -name 'OVMF.fd' 2>/dev/null | head -n 1)
fi

if [ -n "$OVMF_PATH" ] && [ -f "$OVMF_PATH" ]; then
    (
        sleep 7
        printf "echo UEFI_BOOT_SUCCESS\n"
        sleep 1
        printf "exit\n"
    ) | timeout 30s qemu-system-x86_64 -bios "$OVMF_PATH" -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE_UEFI" 2>&1 || true
    tr -d '\r' < "$LOGFILE_UEFI" > "${LOGFILE_UEFI}.tmp" && mv "${LOGFILE_UEFI}.tmp" "$LOGFILE_UEFI"

    grep -q "BdsDxe: starting Boot" "$LOGFILE_UEFI" || { echo "FAIL: OVMF UEFI firmware did not load bootloader"; cat "$LOGFILE_UEFI"; exit 1; }
    grep -q "MyOS kernel booted successfully" "$LOGFILE_UEFI" || { echo "FAIL: kernel did not boot under UEFI"; cat "$LOGFILE_UEFI"; exit 1; }
    grep -q "UEFI_BOOT_SUCCESS" "$LOGFILE_UEFI" || { echo "FAIL: shell command failed under UEFI boot"; cat "$LOGFILE_UEFI"; exit 1; }
    echo "[PASS] Session 22 Part 1: UEFI boot with OVMF firmware verified!"
else
    echo "[SKIP] Session 22 Part 1: OVMF.fd firmware file not found on host"
fi

# -------------------------------------------------------------
# Part 2: >4GB RAM Boot and Detection (-m 6144 and -m 8192)
# -------------------------------------------------------------
echo "[TEST] Session 22 Part 2: Booting with >4GB RAM (-m 6144 and -m 8192)..."
(
    sleep 3
    printf "cat /proc/meminfo\n"
    sleep 0.5
    printf "exit\n"
) | timeout 20s qemu-system-x86_64 -vga std -m 6144 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE_RAM6G" 2>&1 || true
tr -d '\r' < "$LOGFILE_RAM6G" > "${LOGFILE_RAM6G}.tmp" && mv "${LOGFILE_RAM6G}.tmp" "$LOGFILE_RAM6G"

grep -q "Total RAM: 6143 MB" "$LOGFILE_RAM6G" || { echo "FAIL: PMM did not detect 6GB RAM in -m 6144 boot"; cat "$LOGFILE_RAM6G"; exit 1; }
echo "[PASS] Session 22 Part 2a: -m 6144 (6GB RAM) detected and mapped!"

(
    sleep 3
    printf "cat /proc/meminfo\n"
    sleep 0.5
    printf "exit\n"
) | timeout 20s qemu-system-x86_64 -vga std -m 8192 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE_RAM8G" 2>&1 || true
tr -d '\r' < "$LOGFILE_RAM8G" > "${LOGFILE_RAM8G}.tmp" && mv "${LOGFILE_RAM8G}.tmp" "$LOGFILE_RAM8G"

grep -q "Total RAM: 8191 MB" "$LOGFILE_RAM8G" || { echo "FAIL: PMM did not detect 8GB RAM in -m 8192 boot"; cat "$LOGFILE_RAM8G"; exit 1; }
echo "[PASS] Session 22 Part 2b: -m 8192 (8GB RAM) detected and mapped!"

# -------------------------------------------------------------
# Part 3: CPU Hardening (NX, SMEP, SMAP, ASLR)
# -------------------------------------------------------------
echo "[TEST] Session 22 Part 3: CPU hardening suite (NX, SMEP, SMAP, ASLR)..."
(
    sleep 3
    printf "hardentest\n"
    sleep 2
    printf "exit\n"
) | timeout 25s qemu-system-x86_64 -cpu max -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE_HARDEN" 2>&1 || true
tr -d '\r' < "$LOGFILE_HARDEN" > "${LOGFILE_HARDEN}.tmp" && mv "${LOGFILE_HARDEN}.tmp" "$LOGFILE_HARDEN"

grep -q "SMEP enabled (CR4.SMEP=1)" "$LOGFILE_HARDEN" || { echo "FAIL: kernel did not enable CR4.SMEP"; cat "$LOGFILE_HARDEN"; exit 1; }
grep -q "SMAP enabled (CR4.SMAP=1)" "$LOGFILE_HARDEN" || { echo "FAIL: kernel did not enable CR4.SMAP"; cat "$LOGFILE_HARDEN"; exit 1; }
grep -q "PASSED: NX bit enforced" "$LOGFILE_HARDEN" || { echo "FAIL: NX bit test failed"; cat "$LOGFILE_HARDEN"; exit 1; }
grep -q "PASSED: ASLR anonymous mmap space verified" "$LOGFILE_HARDEN" || { echo "FAIL: ASLR test failed"; cat "$LOGFILE_HARDEN"; exit 1; }
grep -q "ALL HARDENING TESTS PASSED!" "$LOGFILE_HARDEN" || { echo "FAIL: hardentest suite failed"; cat "$LOGFILE_HARDEN"; exit 1; }
echo "[PASS] Session 22 Part 3: Modern CPU hardening (NX, SMEP, SMAP, ASLR) verified!"

echo "=========================================================="
echo "ALL SESSION 22 TESTS PASSED SUCCESSFULLY!"
echo "=========================================================="

