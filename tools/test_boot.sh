#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."

LOGFILE1=$(mktemp)
LOGFILE2=$(mktemp)
trap 'rm -f "$LOGFILE1" "$LOGFILE2"' EXIT

echo "[TEST] Rebuilding disk image..."
bash tools/build_disk.sh

echo "[TEST] Session 1: Booting and testing commands, VFS, utilities, and regressions..."

(
    sleep 2
    printf "help\n"
    sleep 1
    printf "pwd\n"
    sleep 1
    printf "mkdir /tmp\n"
    sleep 1
    printf "cd /tmp\n"
    sleep 1
    printf "pwd\n"
    sleep 1
    printf "touch myfile.txt\n"
    sleep 1
    printf "ls\n"
    sleep 1
    printf "cp myfile.txt copied.txt\n"
    sleep 1
    printf "mv copied.txt renamed.txt\n"
    sleep 1
    printf "rm myfile.txt\n"
    sleep 1
    printf "ls -l\n"
    sleep 1
    printf "cd /\n"
    sleep 1
    printf "cat /large.txt\n"
    sleep 1
    printf "fstest\n"
    sleep 2
    printf "hello.elf\n"
    sleep 1
    printf "argtest hello world 123\n"
    sleep 1
    printf "forktest.elf\n"
    sleep 2
    printf "forkexec.elf\n"
    sleep 2
    printf "heaptest.elf\n"
    sleep 1
    printf "badwrite.elf\n"
    sleep 1
    printf "exit\n"
) | timeout 35s qemu-system-i386 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE1" 2>&1 || true

cat "$LOGFILE1"

echo "[TEST] Asserting Session 1 results..."

# Core boot and shell assertions
grep -q "BOOT OK" "$LOGFILE1" || { echo "FAIL: 'BOOT OK' not found"; exit 1; }
echo "[PASS] 'BOOT OK' verified"

grep -q "MyOS Userland Shell (sh)" "$LOGFILE1" || { echo "FAIL: Shell welcome not found"; exit 1; }
echo "[PASS] Shell welcome verified"

grep -q "sh:/tmp" "$LOGFILE1" || { echo "FAIL: Prompt did not reflect cwd /tmp"; exit 1; }
echo "[PASS] Shell cwd prompt verified"

# File utilities assertions
grep -i -q "renamed.txt" "$LOGFILE1" || { echo "FAIL: 'renamed.txt' not found after touch/cp/mv/rm/ls"; exit 1; }
echo "[PASS] touch/cp/mv/rm/ls -l verified"

# Multi-cluster read via cat
grep -q "LARGE_FILE_START_" "$LOGFILE1" || { echo "FAIL: 'LARGE_FILE_START_' not found"; exit 1; }
grep -q "_LARGE_FILE_END" "$LOGFILE1" || { echo "FAIL: '_LARGE_FILE_END' not found"; exit 1; }
echo "[PASS] cat file larger than one cluster verified"

# fstest assertions (edge cases, multi-cluster rw, nested dirs, sync)
grep -q "\[fstest\] ENOENT verified" "$LOGFILE1" || { echo "FAIL: ENOENT test failed"; exit 1; }
echo "[PASS] Edge case: open nonexistent file (ENOENT) verified"

grep -q "\[fstest\] EEXIST verified" "$LOGFILE1" || { echo "FAIL: EEXIST test failed"; exit 1; }
echo "[PASS] Edge case: mkdir existing (EEXIST) verified"

grep -q "\[fstest\] ENOTEMPTY verified" "$LOGFILE1" || { echo "FAIL: ENOTEMPTY test failed"; exit 1; }
echo "[PASS] Edge case: rmdir non-empty (ENOTEMPTY) verified"

grep -q "\[fstest\] bad pointer EFAULT verified" "$LOGFILE1" || { echo "FAIL: EFAULT test failed"; exit 1; }
echo "[PASS] Edge case: bad user pointer (EFAULT) verified"

grep -q "\[fstest\] read past EOF verified" "$LOGFILE1" || { echo "FAIL: read past EOF test failed"; exit 1; }
echo "[PASS] Edge case: read past EOF verified"

grep -q "\[fstest\] multi-cluster write and read verified" "$LOGFILE1" || { echo "FAIL: multi-cluster write/read failed"; exit 1; }
echo "[PASS] Multi-cluster file write and read back verified"

grep -q "\[fstest\] nested dirs verified" "$LOGFILE1" || { echo "FAIL: nested dirs test failed"; exit 1; }
echo "[PASS] Nested directories read/write verified"

grep -q "\[fstest\] ALL FSTESTS COMPLETED" "$LOGFILE1" || { echo "FAIL: fstest did not complete successfully"; exit 1; }
echo "[PASS] All fstest assertions passed"

# Non-regression assertions
grep -q "Hello from a REAL ELF binary" "$LOGFILE1" || { echo "FAIL: hello.elf failed"; exit 1; }
echo "[PASS] hello.elf verified"

grep -q "argtest running" "$LOGFILE1" || { echo "FAIL: argtest failed"; exit 1; }
grep -q "argc = 4" "$LOGFILE1" || { echo "FAIL: argtest argc failed"; exit 1; }
grep -q "argv\[0\] = 'argtest'" "$LOGFILE1" || { echo "FAIL: argtest argv[0] failed"; exit 1; }
grep -q "argv\[1\] = 'hello'" "$LOGFILE1" || { echo "FAIL: argtest argv[1] failed"; exit 1; }
grep -q "argv\[2\] = 'world'" "$LOGFILE1" || { echo "FAIL: argtest argv[2] failed"; exit 1; }
grep -q "argv\[3\] = '123'" "$LOGFILE1" || { echo "FAIL: argtest argv[3] failed"; exit 1; }
grep -q "argtest done" "$LOGFILE1" || { echo "FAIL: argtest done failed"; exit 1; }
echo "[PASS] argtest verified"

grep -q "\[forktest\] I am the PARENT" "$LOGFILE1" || { echo "FAIL: forktest parent failed"; exit 1; }
grep -q "\[forktest\] I am the CHILD" "$LOGFILE1" || { echo "FAIL: forktest child failed"; exit 1; }
echo "[PASS] forktest.elf verified"

grep -q "\[forkexec\] parent: child finished" "$LOGFILE1" || { echo "FAIL: forkexec failed"; exit 1; }
echo "[PASS] forkexec.elf verified"

grep -q "Hello from the heap!" "$LOGFILE1" || { echo "FAIL: heaptest failed"; exit 1; }
grep -q "\[heaptest\] done, exiting" "$LOGFILE1" || { echo "FAIL: heaptest done failed"; exit 1; }
echo "[PASS] heaptest.elf verified"

grep -q "PAGE FAULT" "$LOGFILE1" || { echo "FAIL: badwrite page fault not caught"; exit 1; }
echo "[PASS] badwrite.elf protection verified"

# PID 1 respawn
grep -q "respawning sh.elf" "$LOGFILE1" || { echo "FAIL: PID 1 respawn message not found"; exit 1; }
echo "[PASS] PID 1 respawn on shell exit verified"

echo "[TEST] Session 2: Persistence test across QEMU reboot with the SAME disk.img..."

(
    sleep 2
    printf "cat /persist.txt\n"
    sleep 1
    printf "cat /nest1/nest2/test.txt\n"
    sleep 1
    printf "exit\n"
) | timeout 15s qemu-system-i386 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE2" 2>&1 || true

cat "$LOGFILE2"

grep -q "PERSISTENCE_TEST_OK" "$LOGFILE2" || { echo "FAIL: Persistence test failed - content not found after reboot"; exit 1; }
grep -q "nested_ok" "$LOGFILE2" || { echo "FAIL: Nested dir content not found after reboot"; exit 1; }
echo "[PASS] Persistence test verified across QEMU reboot"

echo "[TEST] Session 3: Host-side FAT filesystem integrity check..."

fsck.fat -n disk.img
echo "[PASS] fsck.fat integrity check clean"

mdir -i disk.img ::
mtype -i disk.img ::/persist.txt | grep -q "PERSISTENCE_TEST_OK"
echo "[PASS] Host mtype cross-check verified"

echo "==============================="
echo "ALL STAGE 13 TESTS PASSED!"
echo "==============================="
exit 0
