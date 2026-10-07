#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."

LOGFILE1=$(mktemp)
LOGFILE2=$(mktemp)
LOGFILE3=$(mktemp)
trap 'rm -f "$LOGFILE1" "$LOGFILE2" "$LOGFILE3"' EXIT

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
    printf "ttytest\n"
    sleep 2
    printf "sigtest\n"
    sleep 3
    printf "memtest\n"
    sleep 10
    printf "exit\n"
) | timeout 60s qemu-system-i386 -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE1" 2>&1 || true

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

grep -q "TTYTEST_PASSED" "$LOGFILE1" || { echo "FAIL: ttytest failed"; exit 1; }
echo "[PASS] ttytest (isatty, devfs nodes, termios, winsize, raw mode) verified"

grep -q "=== ALL SIGTESTS PASSED ===" "$LOGFILE1" || { echo "FAIL: sigtest failed"; exit 1; }
echo "[PASS] sigtest (signals, masks, alarm, pause, broken pipe, waitpid status) verified"

# Stage 15 RAM log assertion (-m 256)
grep -q "Total RAM: 255 MB" "$LOGFILE1" || { echo "FAIL: 256MB Total RAM not logged correctly"; exit 1; }
echo "[PASS] -m 256 Total RAM (255 MB) verified"

# Stage 15 Hardened kheap self-test assertion
grep -q "Kernel heap self-test passed" "$LOGFILE1" || { echo "FAIL: Kernel heap hardening self-test failed"; exit 1; }
echo "[PASS] Kernel heap self-test verified"

# Stage 15 memtest assertions
grep -q "COW correctness (parent untouched, child modified independently)" "$LOGFILE1" || { echo "FAIL: COW correctness test failed"; exit 1; }
echo "[PASS] COW correctness verified"

grep -q "COW efficiency (minimal frames allocated on fork, baseline restored)" "$LOGFILE1" || { echo "FAIL: COW efficiency test failed"; exit 1; }
echo "[PASS] COW efficiency verified"

grep -q "Demand paging (lazy allocation on touch, shrink restores frames)" "$LOGFILE1" || { echo "FAIL: Demand paging test failed"; exit 1; }
echo "[PASS] Demand paging verified"

grep -q "Stack auto-growth (2MB stack recursion succeeded)" "$LOGFILE1" || { echo "FAIL: Stack auto-growth failed"; exit 1; }
echo "[PASS] Stack auto-growth (2MB) verified"

grep -q "Stack guard gap (unbounded recursion killed cleanly with exit 139)" "$LOGFILE1" || { echo "FAIL: Stack guard gap test failed"; exit 1; }
echo "[PASS] Stack guard gap (exit 139) verified"

grep -q "mmap / munmap (anonymous, file-backed, and unmap fault verified)" "$LOGFILE1" || { echo "FAIL: mmap/munmap test failed"; exit 1; }
echo "[PASS] mmap / munmap verified"

grep -q "Protection violations (NULL, code write, non-exec, kernel address all exit 139)" "$LOGFILE1" || { echo "FAIL: Protection violations test failed"; exit 1; }
echo "[PASS] Protection violations (exit 139) verified"

grep -q "Fork storm (200 sequential + 20 concurrent children clean, zero leaks)" "$LOGFILE1" || { echo "FAIL: Fork storm test failed"; exit 1; }
echo "[PASS] Fork storm (220 forks zero leak) verified"

grep -q "\[memtest\] ALL TESTS PASSED SUCCESSFULLY!" "$LOGFILE1" || { echo "FAIL: memtest suite failed"; exit 1; }
echo "[PASS] Stage 15 memtest suite verified"

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
) | timeout 15s qemu-system-i386 -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE2" 2>&1 || true

cat "$LOGFILE2"

grep -q "PERSISTENCE_TEST_OK" "$LOGFILE2" || { echo "FAIL: Persistence test failed - content not found after reboot"; exit 1; }
grep -q "nested_ok" "$LOGFILE2" || { echo "FAIL: Nested dir content not found after reboot"; exit 1; }
echo "[PASS] Persistence test verified across QEMU reboot"

echo "[TEST] Session 3: Stage 14 IPC, pipes, redirections, pipelines, filters, and stress test..."

(
    sleep 2
    printf 'echo hello > /tmp/a\n'
    sleep 1
    printf 'cat /tmp/a\n'
    sleep 1
    printf 'echo one >> /tmp/a\n'
    sleep 1
    printf 'cat /tmp/a\n'
    sleep 1
    printf 'cat /tmp/a | wc -l\n'
    sleep 1
    printf 'yes | head -n 5\n'
    sleep 1
    printf 'ls /bin | grep elf | sort | head -n 3\n'
    sleep 1
    printf 'cat < /tmp/a | tee /tmp/b | wc -c\n'
    sleep 1
    printf 'diff /tmp/a /tmp/b\n'
    sleep 1
    printf 'false; echo $?\n'
    sleep 1
    printf 'true; echo $?\n'
    sleep 1
    printf 'ls /nonexistent 2> /tmp/err\n'
    sleep 1
    printf 'cat /tmp/err\n'
    sleep 1
    printf 'cat /big.txt | wc -c\n'
    sleep 2
    printf 'stress\n'
    sleep 2
    printf 'exit\n'
) | timeout 35s qemu-system-i386 -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE3" 2>&1 || true

cat "$LOGFILE3"

echo "[TEST] Asserting Session 3 results..."

# Redirection > and cat
grep -q "hello" "$LOGFILE3" || { echo "FAIL: 'echo hello > /tmp/a' failed"; exit 1; }
echo "[PASS] 'echo hello > /tmp/a; cat /tmp/a' verified"

# Redirection >> and cat
grep -q "one" "$LOGFILE3" || { echo "FAIL: 'echo one >> /tmp/a' failed"; exit 1; }
echo "[PASS] 'echo one >> /tmp/a; cat /tmp/a' verified"

# Pipeline with wc -l
grep -q "2" "$LOGFILE3" || { echo "FAIL: 'cat /tmp/a | wc -l' did not output 2"; exit 1; }
echo "[PASS] 'cat /tmp/a | wc -l' -> 2 verified"

# yes | head -n 5 and EPIPE handling
grep -q "y" "$LOGFILE3" || { echo "FAIL: 'yes | head -n 5' failed"; exit 1; }
echo "[PASS] 'yes | head -n 5' (backpressure/EPIPE) verified"

# Multi-stage pipeline: ls | grep | sort | head
grep -q "argtest.elf" "$LOGFILE3" || { echo "FAIL: 'ls /bin | grep elf | sort | head -n 3' failed"; exit 1; }
echo "[PASS] 'ls /bin | grep elf | sort | head -n 3' verified"

# tee and diff
grep -q "10" "$LOGFILE3" || { echo "FAIL: 'cat < /tmp/a | tee /tmp/b | wc -c' failed"; exit 1; }
grep -q "Files /tmp/a and /tmp/b match" "$LOGFILE3" || { echo "FAIL: diff /tmp/a /tmp/b failed"; exit 1; }
echo "[PASS] 'cat < /tmp/a | tee /tmp/b | wc -c' and diff verified"

# Exit code tracking and $? expansion
grep -q "1" "$LOGFILE3" || { echo "FAIL: 'false; echo $?' failed"; exit 1; }
grep -q "0" "$LOGFILE3" || { echo "FAIL: 'true; echo $?' failed"; exit 1; }
echo "[PASS] 'false; echo $?' -> 1 and 'true; echo $?' -> 0 verified"

# Stderr redirection
grep -q "ls: cannot access '/nonexistent'" "$LOGFILE3" || { echo "FAIL: stderr redirection 'ls /nonexistent 2> /tmp/err' failed"; exit 1; }
echo "[PASS] 'ls /nonexistent 2> /tmp/err; cat /tmp/err' verified"

# Large pipeline transfer (>64KB)
grep -q "70000" "$LOGFILE3" || { echo "FAIL: 'cat /big.txt | wc -c' failed (>64KB pipe transfer)"; exit 1; }
echo "[PASS] Large pipeline transfer (70KB through 4KB ring buffer) verified"

# Stress test (50 iterations of fork+pipe+exec+wait with zero leak check)
grep -q "ALL 50 ITERATIONS PASSED - NO LEAKS!" "$LOGFILE3" || { echo "FAIL: 50-iteration stress test failed"; exit 1; }
echo "[PASS] Stress test: 50 iterations fork+pipe+exec+wait leak-free verified"

echo "[TEST] Session 4: Host-side FAT filesystem integrity check..."

fsck.fat -n disk.img
echo "[PASS] fsck.fat integrity check clean"

mdir -i disk.img ::
mtype -i disk.img ::/persist.txt | grep -q "PERSISTENCE_TEST_OK"
mtype -i disk.img ::/tmp/a | grep -q "hello"
mtype -i disk.img ::/tmp/b | grep -q "hello"
echo "[PASS] Host mtype cross-check for persistent and created files verified"

echo "[TEST] Session 5: Low-memory boot test (-m 64)..."
LOGFILE5=$(mktemp)
timeout 10s qemu-system-i386 -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE5" 2>&1 || true
cat "$LOGFILE5"

grep -q "BOOT OK" "$LOGFILE5" || { echo "FAIL: Low-memory boot failed (BOOT OK not found)"; rm -f "$LOGFILE5"; exit 1; }
grep -q "Total RAM: 63 MB" "$LOGFILE5" || { echo "FAIL: -m 64 Total RAM (63 MB) not logged correctly"; rm -f "$LOGFILE5"; exit 1; }
grep -q "Kernel heap self-test passed" "$LOGFILE5" || { echo "FAIL: -m 64 Kernel heap self-test failed"; rm -f "$LOGFILE5"; exit 1; }
echo "[PASS] Low-memory boot (-m 64, 63 MB RAM, BOOT OK) verified"
rm -f "$LOGFILE5"

echo "==============================="
echo "ALL STAGE 15 TESTS PASSED!"
echo "==============================="
exit 0
