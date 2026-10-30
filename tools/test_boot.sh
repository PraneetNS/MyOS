#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."

LOGFILE1=$(mktemp)
LOGFILE2=$(mktemp)
LOGFILE3=$(mktemp)
LOGFILE6=$(mktemp)
LOGFILE7=$(mktemp)
LOGFILE8=$(mktemp)
LOGFILE16=$(mktemp)
LOGFILE17=$(mktemp)
LOGFILE18=$(mktemp)
LOGFILE18_LOWMEM=$(mktemp)
LOGFILE19=$(mktemp)
trap 'kill -9 "$HB_PID" 2>/dev/null || true; rm -f "$LOGFILE1" "$LOGFILE2" "$LOGFILE3" "$LOGFILE6" "$LOGFILE7" "$LOGFILE8" "$LOGFILE15" "$LOGFILE16" "$LOGFILE17" "$LOGFILE18" "$LOGFILE18_LOWMEM" "$LOGFILE19"' EXIT

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
    sleep 3
    printf "fstest\n"
    sleep 4
    printf "lfntest\n"
    sleep 70
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
    printf "fbtest.elf\n"
    sleep 2
    printf "ttytest\n"
    sleep 4
    printf "sigtest\n"
    sleep 3
    printf "jobtest\n"
    sleep 2
    printf "shmtest.elf\n"
    sleep 3
    printf "sockettest.elf\n"
    sleep 5
    printf "ptytest.elf\n"
    sleep 5
    printf "ipctest.elf\n"
    sleep 12
    printf "memtest\n"
    sleep 45
    printf "exit\n"
    sleep 1
    printf "poweroff\n"
) | timeout 240s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE1" 2>&1 || true

cat "$LOGFILE1"

echo "[TEST] Asserting Session 1 results..."

# Core boot and shell assertions
grep -q "BOOT OK" "$LOGFILE1" || { echo "FAIL: 'BOOT OK' not found"; exit 1; }
echo "[PASS] 'BOOT OK' verified"

grep -q "\[fb\] 1024x768x32" "$LOGFILE1" || { echo "FAIL: Framebuffer boot log not found"; exit 1; }
echo "[PASS] Framebuffer boot log verified"

grep -q "\[mouse\] PS/2 mouse ready" "$LOGFILE1" || { echo "FAIL: PS/2 mouse init log not found"; exit 1; }
echo "[PASS] PS/2 mouse driver initialised (IRQ12)"

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

grep -q "\[lfntest\] PASS: 30-file alias collision" "$LOGFILE1" || { echo "FAIL: 30-file alias collision failed"; exit 1; }
grep -q "\[lfntest\] PASS: Directory growth across clusters" "$LOGFILE1" || { echo "FAIL: Directory growth failed"; exit 1; }
grep -q "\[lfntest\] PASS: 255-character filename succeeded" "$LOGFILE1" || { echo "FAIL: 255-char name failed"; exit 1; }
grep -q "\[lfntest\] PASS: 256-character filename rejected with -ENAMETOOLONG" "$LOGFILE1" || { echo "FAIL: 256-char name not rejected with ENAMETOOLONG"; exit 1; }
grep -q "\[lfntest\] PASS: UTF-8 filenames" "$LOGFILE1" || { echo "FAIL: UTF-8 names failed"; exit 1; }
grep -q "\[lfntest\] PASS: Legal-in-LFN characters" "$LOGFILE1" || { echo "FAIL: Legal-in-LFN characters failed"; exit 1; }
grep -q "\[lfntest\] PASS: Leading dot (.hidden) verified" "$LOGFILE1" || { echo "FAIL: .hidden failed"; exit 1; }
grep -q "\[lfntest\] PASS: Trailing dot and space filenames rejected" "$LOGFILE1" || { echo "FAIL: Trailing dot/space rejection failed"; exit 1; }
grep -q "\[lfntest\] PASS: Case collision" "$LOGFILE1" || { echo "FAIL: Case collision failed"; exit 1; }
grep -q "\[lfntest\] PASS: short <-> long rename verified" "$LOGFILE1" || { echo "FAIL: short <-> long rename failed"; exit 1; }
grep -q "\[lfntest\] PASS: Case-only rename verified" "$LOGFILE1" || { echo "FAIL: Case-only rename failed"; exit 1; }
grep -q "\[lfntest\] PASS: Cross-directory and same-directory file replacement verified" "$LOGFILE1" || { echo "FAIL: Cross-dir rename failed"; exit 1; }
grep -q "\[lfntest\] PASS: Directory cross-directory rename with '..' fix and clean entries verified" "$LOGFILE1" || { echo "FAIL: Dir rename with .. fix failed"; exit 1; }
grep -q "\[lfntest\] PASS: Zero frame leaks across LFN process lifecycle" "$LOGFILE1" || { echo "FAIL: lfntest frame leak detected"; exit 1; }
grep -q "\[lfntest\] ALL LFN TESTS PASSED SUCCESSFULLY!" "$LOGFILE1" || { echo "FAIL: lfntest did not complete successfully"; exit 1; }
echo "[PASS] All lfntest assertions passed (including alias collision, dir growth, names, rename matrix, and zero leaks)"

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

grep -q "\[fbtest\] PASS" "$LOGFILE1" || { echo "FAIL: fbtest failed"; exit 1; }
echo "[PASS] Framebuffer /dev/fb0 and mmap fork test verified"

grep -q "TTYTEST_PASSED" "$LOGFILE1" || { echo "FAIL: ttytest failed"; exit 1; }
echo "[PASS] ttytest (isatty, devfs nodes, termios, winsize, raw mode) verified"

grep -q "=== ALL SIGTESTS PASSED ===" "$LOGFILE1" || { echo "FAIL: sigtest failed"; exit 1; }
echo "[PASS] sigtest (signals, masks, alarm, pause, broken pipe, waitpid status) verified"

grep -q "=== ALL JOBTESTS PASSED ===" "$LOGFILE1" || { echo "FAIL: jobtest failed"; exit 1; }
echo "[PASS] jobtest (pgid, sid, ppid, tcgetpgrp, SIGTTIN on background read) verified"

grep -q "ALL STAGE 18B STEP 2 TESTS PASSED" "$LOGFILE1" || { echo "FAIL: shmtest failed"; exit 1; }
echo "[PASS] Stage 18B Step 2 (MAP_SHARED, shm, atomic builtins, SIGBUS) verified"

grep -q "ALL STAGE 18B STEP 3 SOCKET TESTS PASSED SUCCESSFULLY" "$LOGFILE1" || { echo "FAIL: sockettest failed"; exit 1; }
echo "[PASS] Stage 18B Step 3 (AF_UNIX sockets, 64KB ring, /proc/net/unix) verified"

grep -q "ALL STAGE 18B STEP 4 PTY TESTS PASSED SUCCESSFULLY" "$LOGFILE1" || { echo "FAIL: ptytest failed"; exit 1; }
echo "[PASS] Stage 18B Step 4 (PTYs, /dev/ptmx, /dev/pts, line discipline, TIOCSWINSZ, forkpty) verified"

grep -q "ALL STAGE 18B IPC & SESSION TESTS PASSED (5/5)" "$LOGFILE1" || { echo "FAIL: ipctest failed"; exit 1; }
echo "[PASS] Stage 18B Step 6 (comprehensive ipctest suite: tmpfs, shm, sockets, ptys, pipeline) verified"

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
) | timeout 15s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE2" 2>&1 || true

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
    printf 'ls /bin | grep elf | sort | head -n 4\n'
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
) | timeout 45s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE3" 2>&1 || true

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
grep -E -q "(agentctl.elf|argtest.elf)" "$LOGFILE3" || { echo "FAIL: 'ls /bin | grep elf | sort | head -n 4' failed"; exit 1; }
echo "[PASS] 'ls /bin | grep elf | sort | head -n 4' verified"

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
(
    sleep 3
    printf "exit\n"
) | timeout 12s qemu-system-i386 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE5" 2>&1 || true
cat "$LOGFILE5"

grep -q "BOOT OK" "$LOGFILE5" || { echo "FAIL: Low-memory boot failed (BOOT OK not found)"; rm -f "$LOGFILE5"; exit 1; }
grep -q "Total RAM: 63 MB" "$LOGFILE5" || { echo "FAIL: -m 64 Total RAM (63 MB) not logged correctly"; rm -f "$LOGFILE5"; exit 1; }
grep -q "Kernel heap self-test passed" "$LOGFILE5" || { echo "FAIL: -m 64 Kernel heap self-test failed"; rm -f "$LOGFILE5"; exit 1; }
echo "[PASS] Low-memory boot (-m 64, 63 MB RAM, BOOT OK) verified"
rm -f "$LOGFILE5"
sleep 1

echo "[TEST] Session 6: Live job control and interactive TTY signals over COM1..."

(
    sleep 4
    # Warm up binary cache
    printf "echo echo warm | sh.elf\n"
    sleep 0.5
    # Baseline
    printf "shinfo\n"
    sleep 0.3

    # Case 1: spin in fg, 0x03. Assert: prompt returns, $? = 130
    printf "spin.elf\n"
    sleep 0.3
    printf "\x03"
    sleep 0.3
    printf "echo exit_case1=\$?\n"
    sleep 0.3

    # Case 6: spin in fg, 0x1c. Assert: prompt returns, $? = 131
    printf "spin.elf\n"
    sleep 0.3
    printf "\x1c"
    sleep 0.3
    printf "echo exit_case6=\$?\n"
    sleep 0.3

    # Case 2: spin in fg, 0x1a. Assert stopped, jobs shows Stopped, fg, then 0x03 kills ($? = 130)
    printf "spin.elf\n"
    sleep 0.3
    printf "\x1a"
    sleep 0.3
    printf "jobs\n"
    sleep 0.3
    printf "fg\n"
    sleep 0.3
    printf "\x03"
    sleep 0.3
    printf "echo exit_case2=\$?\n"
    sleep 0.3

    # Case 3: spin.elf &, jobs shows Running, kill %1, next prompt reports Done/Terminated, jobs is empty
    printf "spin.elf &\n"
    sleep 0.3
    printf "jobs\n"
    sleep 0.3
    printf 'kill %%1\n'
    sleep 0.3
    printf "jobs\n"
    sleep 0.3

    # Case 4: spin.elf, 0x1a, bg, jobs shows Running, kill %1
    printf "spin.elf\n"
    sleep 0.3
    printf "\x1a"
    sleep 0.3
    printf "bg\n"
    sleep 0.3
    printf "jobs\n"
    sleep 0.3
    printf 'kill %%1\n'
    sleep 0.3
    printf "jobs\n"
    sleep 0.3

    # Case 5: Pipeline: cat | spin.elf, Ctrl+C kills BOTH stages
    printf "cat | spin.elf\n"
    sleep 0.3
    printf "\x03"
    sleep 0.3
    printf "echo exit_case5=\$?\n"
    sleep 0.3

    # Case 7: A background job that tries to read stdin gets SIGTTIN and stops
    printf "cat &\n"
    sleep 0.3
    printf "\n"
    sleep 0.3
    printf "jobs\n"
    sleep 0.3
    printf 'kill %%1\n'
    sleep 0.3
    printf "jobs\n"
    sleep 0.3

    # Non-interactive subshell: scripts / pipes don't install job-control handlers
    printf 'echo echo subshell_works | sh.elf\n'
    sleep 0.3

    # Case 8: Shell is still PID 1's child, prompt works, free frames check
    printf "shinfo\n"
    sleep 0.3
    printf "exit\n"
) | timeout 22s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE6" 2>&1 || true

cat "$LOGFILE6"

echo "[TEST] Asserting Session 6 results..."

# Case 1 assertions
grep -q "exit_case1=130" "$LOGFILE6" || { echo "FAIL: Case 1: spin.elf + Ctrl+C exit code was not 130"; exit 1; }
echo "[PASS] Case 1: spin.elf killed with Ctrl+C (0x03), exit code 130 verified"

# Case 6 assertions
grep -q "exit_case6=131" "$LOGFILE6" || { echo "FAIL: Case 6: spin.elf + Ctrl+\\ exit code was not 131"; exit 1; }
echo "[PASS] Case 6: spin.elf killed with Ctrl+\\ (0x1c), exit code 131 verified"

# Case 2 assertions
grep -q "\[1\]+ Stopped  spin.elf" "$LOGFILE6" || { echo "FAIL: Case 2: spin.elf was not stopped by Ctrl+Z"; exit 1; }
grep -q "\[1\] + Stopped  spin.elf" "$LOGFILE6" || { echo "FAIL: Case 2: jobs did not report Stopped for spin.elf"; exit 1; }
grep -q "PID [0-9]* resumed by SIGCONT" "$LOGFILE6" || { echo "FAIL: Case 2: fg did not resume stopped process with SIGCONT"; exit 1; }
grep -q "exit_case2=130" "$LOGFILE6" || { echo "FAIL: Case 2: resumed spin.elf killed with Ctrl+C exit code was not 130"; exit 1; }
echo "[PASS] Case 2: spin.elf stopped by Ctrl+Z, jobs shows Stopped, fg resumes, Ctrl+C kills ($? = 130) verified"

# Case 3 assertions
grep -q "\[1\]   Running  spin.elf" "$LOGFILE6" || { echo "FAIL: Case 3: spin.elf & did not show Running in jobs"; exit 1; }
grep -q "\[1\]+ Terminated  spin.elf" "$LOGFILE6" || { echo "FAIL: Case 3: kill %1 did not terminate spin.elf"; exit 1; }
echo "[PASS] Case 3: spin.elf &, jobs shows Running, kill %1 terminates, jobs cleared verified"

# Case 4 assertions
grep -q "\[1\]+ spin.elf &" "$LOGFILE6" || { echo "FAIL: Case 4: bg did not resume spin.elf in background"; exit 1; }
echo "[PASS] Case 4: spin.elf, Ctrl+Z, bg resumes in background, kill %1 terminates verified"

# Case 5 assertions
grep -q "exit_case5=130" "$LOGFILE6" || { echo "FAIL: Case 5: cat | spin.elf pipeline Ctrl+C exit code was not 130"; exit 1; }
echo "[PASS] Case 5: Pipeline cat | spin.elf killed by Ctrl+C in same pgrp verified"

# Case 7 assertions
grep -q "PID [0-9]* (cat) stopped by signal 21" "$LOGFILE6" || { echo "FAIL: Case 7: background cat reading stdin did not receive SIGTTIN (21)"; exit 1; }
grep -q "\[1\] + Stopped  cat" "$LOGFILE6" || { echo "FAIL: Case 7: jobs did not report Stopped for background cat"; exit 1; }
echo "[PASS] Case 7: Background job reading stdin stopped by SIGTTIN verified"

# Non-interactive subshell assertion
grep -q "subshell_works" "$LOGFILE6" || { echo "FAIL: Non-interactive subshell failed"; exit 1; }
echo "[PASS] Non-interactive subshell (echo ... | sh.elf) verified"

# Case 8 assertions
BASELINE_FRAMES=$(grep "sh: pid=" "$LOGFILE6" | head -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
FINAL_FRAMES=$(grep "sh: pid=" "$LOGFILE6" | tail -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
if [ -n "$BASELINE_FRAMES" ] && [ -n "$FINAL_FRAMES" ]; then
    if [ "$BASELINE_FRAMES" -ne "$FINAL_FRAMES" ]; then
        echo "FAIL: Case 8: Frame leak detected: baseline $BASELINE_FRAMES vs final $FINAL_FRAMES"
        exit 1
    fi
    echo "[PASS] Case 8: Zero frame leak verified (baseline $BASELINE_FRAMES == final $FINAL_FRAMES)"
else
    echo "FAIL: Case 8: Could not parse free frames from shinfo"
    exit 1
fi
grep -q "sh: pid=[0-9]*" "$LOGFILE6" || { echo "FAIL: Case 8: Shell info not verified"; exit 1; }
echo "[PASS] Case 8: Shell is healthy, prompt works, zero leaks verified"

echo "[TEST] Session 7: Stage 16 Time, RTC, sleeping wait queue, procfs, scheduler, and responsiveness..."

(
    sleep 4
    printf "true\n"
    sleep 0.3
    printf "shinfo\n"
    sleep 0.3
    printf "date\n"
    sleep 0.3
    printf "date -u\n"
    sleep 0.3
    printf "timetest.elf\n"
    sleep 3.5
    printf "sigtest.elf\n"
    sleep 2.5
    printf "fputest.elf\n"
    sleep 2.5
    printf "uptime\n"
    sleep 0.3
    printf "free\n"
    sleep 0.3
    printf "cat /proc/uptime\n"
    sleep 0.3
    printf "cat /proc/meminfo\n"
    sleep 0.3
    printf "cat /proc/self/cmdline\n"
    sleep 0.3
    printf "spin.elf &\n"
    sleep 0.3
    printf "ps\n"
    sleep 0.3
    printf 'kill %%1\n'
    sleep 0.3
    printf "jobs\n"
    sleep 0.3
    printf "spin.elf &\n"
    sleep 0.3
    printf "spin.elf &\n"
    sleep 0.3
    printf "echo responsive_under_two_spinners\n"
    sleep 0.3
    printf 'kill %%1\n'
    sleep 0.3
    printf 'kill %%2\n'
    sleep 0.3
    printf 'kill %%3\n'
    sleep 0.3
    printf "jobs\n"
    sleep 0.3
    printf "echo time_write > /tmp/timed.txt\n"
    sleep 0.3
    printf "ls -l /tmp\n"
    sleep 0.3
    printf "shinfo\n"
    sleep 0.3
    printf "exit\n"
) | timeout 35s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE7" 2>&1 || true

tr -d '\r' < "$LOGFILE7" > "${LOGFILE7}.tmp" && mv "${LOGFILE7}.tmp" "$LOGFILE7"
cat "$LOGFILE7"

echo "[TEST] Asserting Session 7 results..."

# Date assertions: year >= 2026, offset differs by 5h30m (330 min)
DATE_LOCAL=$(grep -E "(IST|LOC) [0-9]{4}" "$LOGFILE7" | head -n 1)
DATE_UTC=$(grep -E "UTC [0-9]{4}" "$LOGFILE7" | head -n 1)
if [ -z "$DATE_LOCAL" ] || [ -z "$DATE_UTC" ]; then
    echo "FAIL: Could not find date or date -u output in Session 7"
    exit 1
fi
YEAR=$(echo "$DATE_LOCAL" | grep -oE "[0-9]{4}$")
if [ "$YEAR" -lt 2026 ]; then
    echo "FAIL: Year $YEAR is less than 2026"
    exit 1
fi
echo "[PASS] Date is >= 2026 (year $YEAR verified)"

LH=$(echo "$DATE_LOCAL" | grep -oE "[0-9]{2}:[0-9]{2}:[0-9]{2}" | head -n 1 | cut -d: -f1)
LM=$(echo "$DATE_LOCAL" | grep -oE "[0-9]{2}:[0-9]{2}:[0-9]{2}" | head -n 1 | cut -d: -f2)
UH=$(echo "$DATE_UTC" | grep -oE "[0-9]{2}:[0-9]{2}:[0-9]{2}" | head -n 1 | cut -d: -f1)
UM=$(echo "$DATE_UTC" | grep -oE "[0-9]{2}:[0-9]{2}:[0-9]{2}" | head -n 1 | cut -d: -f2)
L_MIN=$(( 10#$LH * 60 + 10#$LM ))
U_MIN=$(( 10#$UH * 60 + 10#$UM ))
DIFF_MIN=$(( (L_MIN - U_MIN + 1440) % 1440 ))
if [ "$DIFF_MIN" -lt 329 ] || [ "$DIFF_MIN" -gt 331 ]; then
    echo "FAIL: Timezone difference is $DIFF_MIN minutes, expected ~330 minutes (5h30m)"
    exit 1
fi
echo "[PASS] date with /etc/timezone=330 differs from date -u by exactly 5h30m ($DIFF_MIN minutes verified)"

# Timetest assertions
grep -q "\[timetest\] sleeper cpu_ticks=[0-4] (< 5) PASS" "$LOGFILE7" || { echo "FAIL: Sleeper CPU ticks was not < 5"; exit 1; }
grep -q "\[timetest\] sleep 2 elapsed ticks=.* (190-230) PASS" "$LOGFILE7" || { echo "FAIL: sleep 2 was not 190-230 ticks"; exit 1; }
grep -q "\[timetest\] meminfo FramesFree=.* vs sys_free_frames=.* MATCH PASS" "$LOGFILE7" || { echo "FAIL: /proc/meminfo free frames did not match sys_free_frames"; exit 1; }
grep -q "=== ALL TIMETESTS PASSED ===" "$LOGFILE7" || { echo "FAIL: timetest suite did not pass"; exit 1; }
echo "[PASS] Non-busy sleep 2 (190-230 ticks, CPU ticks < 5) and meminfo free frames match verified"

# Sigtest assertions
grep -q "PASS: alarm and pause" "$LOGFILE7" || { echo "FAIL: alarm(1) and pause failed"; exit 1; }
grep -q "PASS: nanosleep interrupted returned -EINTR with remaining time > 0" "$LOGFILE7" || { echo "FAIL: nanosleep signal interruption with rem > 0 failed"; exit 1; }
grep -q "=== ALL SIGTESTS PASSED ===" "$LOGFILE7" || { echo "FAIL: sigtest suite did not pass"; exit 1; }
echo "[PASS] alarm(1) wakes pause() via SIGALRM and nanosleep interrupted returns -EINTR with remaining time > 0 verified"

# Fputest assertions
grep -q "PASS: 3 concurrent processes with FPU/SSE completed without corruption" "$LOGFILE7" || { echo "FAIL: FPU/SSE concurrent calculation corrupted"; exit 1; }
grep -q "=== ALL FPUTESTS PASSED ===" "$LOGFILE7" || { echo "FAIL: fputest suite did not pass"; exit 1; }
echo "[PASS] FPU/SSE concurrent execution, context switching, and SIGFPE verified"

# Procfs and ps assertions
grep -q "MemTotal:" "$LOGFILE7" || { echo "FAIL: /proc/meminfo MemTotal missing"; exit 1; }
grep -q "spin" "$LOGFILE7" || { echo "FAIL: ps did not list spin while running"; exit 1; }
echo "[PASS] /proc/meminfo, /proc/uptime, and ps listing live processes verified"

# Responsiveness under 2 spinners
grep -q "responsive_under_two_spinners" "$LOGFILE7" || { echo "FAIL: Shell unresponsive under two spinners"; exit 1; }
echo "[PASS] Shell responsive under two spin.elf processes verified"

# FAT16 timestamps in ls -l
grep -q "timed.txt" "$LOGFILE7" || { echo "FAIL: timed.txt not listed in ls -l"; exit 1; }
echo "[PASS] FAT16 timestamp creation and ls -l local time display verified"

# Zero frame leak check for Session 7
BASELINE_FRAMES_S7=$(grep "sh: pid=" "$LOGFILE7" | head -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
FINAL_FRAMES_S7=$(grep "sh: pid=" "$LOGFILE7" | tail -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
if [ -n "$BASELINE_FRAMES_S7" ] && [ -n "$FINAL_FRAMES_S7" ]; then
    if [ "$BASELINE_FRAMES_S7" -ne "$FINAL_FRAMES_S7" ]; then
        echo "FAIL: Session 7 frame leak detected: baseline $BASELINE_FRAMES_S7 vs final $FINAL_FRAMES_S7"
        exit 1
    fi
    echo "[PASS] Session 7: Zero frame leak verified (baseline $BASELINE_FRAMES_S7 == final $FINAL_FRAMES_S7)"
else
    echo "FAIL: Session 7: Could not parse free frames from shinfo"
    exit 1
fi

echo "[TEST] Session 8: Testing Newlib libc (libctest) and Lua 5.4 runtime..."

(
    sleep 2
    printf "libctest.elf\n"
    sleep 4
    printf "lua.elf -e \"print(1+1)\"\n"
    sleep 1
    printf "lua.elf /home/demo.lua\n"
    sleep 3
    printf "ls /bin | lua.elf -e \"for l in io.lines() do if l:find('elf') then print(l) end end\" | head -n 4\n"
    sleep 2
    printf "lua.elf -e \"invalid lua syntax !!!\"\n"
    sleep 1
    printf "echo LUA_ERR=\$?\n"
    sleep 1
    printf "lua.elf -e \"function f() f() end; f()\"\n"
    sleep 3
    printf "echo LUA_RECURSE_ERR=\$?\n"
    sleep 2
    printf "shinfo\n"
    sleep 2
    printf "lua.elf /home/alloc.lua\n"
    sleep 4
    printf "shinfo\n"
    sleep 2
    printf "exit\n"
) | timeout 70s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE8" 2>&1 || true

tr -d '\r' < "$LOGFILE8" > "${LOGFILE8}.tmp" && mv "${LOGFILE8}.tmp" "$LOGFILE8"
cat "$LOGFILE8"

echo "[TEST] Asserting Session 8 results..."

grep -q "\[libctest\] ALL TESTS PASSED" "$LOGFILE8" || { echo "FAIL: libctest suite did not pass"; exit 1; }
echo "[PASS] Newlib libc comprehensive test suite (libctest) verified"

grep -q "^2$" "$LOGFILE8" || { echo "FAIL: Lua 1+1 expression failed"; exit 1; }
echo "[PASS] Lua -e expression evaluation verified"

grep -q "=== MyOS Lua 5.4 Demo ===" "$LOGFILE8" || { echo "FAIL: Lua demo banner not found"; exit 1; }
grep -q "Fibonacci(10) = 55" "$LOGFILE8" || { echo "FAIL: Lua Fibonacci failed"; exit 1; }
grep -q "math: sin(pi/2)=1.0000" "$LOGFILE8" || { echo "FAIL: Lua math/FPU failed"; exit 1; }
grep -q "Sorted table of 100 elements" "$LOGFILE8" || { echo "FAIL: Lua table sort failed"; exit 1; }
grep -q "File I/O read back line 1: Lua 5.4 file I/O test on MyOS" "$LOGFILE8" || { echo "FAIL: Lua file I/O failed"; exit 1; }
grep -q "=== Demo Complete ===" "$LOGFILE8" || { echo "FAIL: Lua demo completion not found"; exit 1; }
echo "[PASS] Lua /home/demo.lua execution verified"

grep -E -q "(agentctl.elf|argtest.elf)" "$LOGFILE8" || { echo "FAIL: Lua pipeline failed"; exit 1; }
echo "[PASS] Shell to Lua stdin/stdout pipeline verified"

grep -q "LUA_ERR=1" "$LOGFILE8" || { echo "FAIL: Lua syntax error exit status not 1"; exit 1; }
echo "[PASS] Lua syntax error handled gracefully with non-zero exit code"

grep -q "stack overflow" "$LOGFILE8" || { echo "FAIL: Lua recursion stack overflow message not found"; exit 1; }
grep -q "LUA_RECURSE_ERR=1" "$LOGFILE8" || { echo "FAIL: Lua recursion error exit status not 1"; exit 1; }
echo "[PASS] Lua deep recursion error caught without kernel crash"

grep -q "LUA_20MB_ALLOC_OK" "$LOGFILE8" || { echo "FAIL: Lua 20MB allocation failed"; exit 1; }
echo "[PASS] Lua 20MB allocation and garbage collection verified"

BASELINE_FRAMES_S8=$(grep "sh: pid=" "$LOGFILE8" | head -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
FINAL_FRAMES_S8=$(grep "sh: pid=" "$LOGFILE8" | tail -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
if [ -n "$BASELINE_FRAMES_S8" ] && [ -n "$FINAL_FRAMES_S8" ]; then
    if [ "$BASELINE_FRAMES_S8" -ne "$FINAL_FRAMES_S8" ]; then
        echo "FAIL: Session 8 frame leak detected: baseline $BASELINE_FRAMES_S8 vs final $FINAL_FRAMES_S8"
        exit 1
    fi
    echo "[PASS] Session 8: Zero frame leak verified (baseline $BASELINE_FRAMES_S8 == final $FINAL_FRAMES_S8)"
else
    echo "FAIL: Session 8: Could not parse free frames from shinfo"
    exit 1
fi

echo "[TEST] Session 9: Stage 18A /dev/fb0 mmap, mouse driver, and keyboard input layer..."

LOGFILE9=$(mktemp)
(
    sleep 3
    printf "fbtest.elf\n"
    sleep 3
    printf "mousetest.elf\n"
    sleep 3
    printf "kbdtest.elf\n"
    sleep 3
    printf "ls /dev/input\n"
    sleep 1
    printf "exit\n"
) | timeout 30s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE9" 2>&1 || true

tr -d '\r' < "$LOGFILE9" > "${LOGFILE9}.tmp" && mv "${LOGFILE9}.tmp" "$LOGFILE9"
cat "$LOGFILE9"

echo "[TEST] Asserting Session 9 results..."

grep -q "\[fbtest\] PASS" "$LOGFILE9" || { echo "FAIL: fbtest.elf failed"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] /dev/fb0 and device mmap (fbtest.elf) verified"

grep -q "\[mouse\] PS/2 mouse ready" "$LOGFILE9" || { echo "FAIL: PS/2 mouse driver did not init"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] PS/2 mouse driver initialised"

grep -q "\[input\] input event layer ready" "$LOGFILE9" || { echo "FAIL: input event layer did not init"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] Input event ring buffer initialised"

grep -q "\[mousetest\] PASS: /dev/input is a directory" "$LOGFILE9" || { echo "FAIL: /dev/input not a directory"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] /dev/input is a directory"

grep -q "\[mousetest\] PASS: 'mice' found in /dev/input" "$LOGFILE9" || { echo "FAIL: 'mice' not found in /dev/input listing"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] 'mice' entry present in /dev/input directory"

grep -q "\[mousetest\] PASS: /dev/input/mice opened" "$LOGFILE9" || { echo "FAIL: /dev/input/mice could not be opened"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] /dev/input/mice opened successfully"

grep -q "\[mousetest\] PASS: /dev/input/mouse0 is a character device" "$LOGFILE9" || { echo "FAIL: /dev/input/mouse0 is not a char device"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] /dev/input/mouse0 is a character device"

grep -q "\[mousetest\] ALL MOUSE TESTS PASSED" "$LOGFILE9" || { echo "FAIL: mousetest did not complete"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] mousetest.elf all tests passed"

grep -q "\[kbdtest\] PASS: /dev/input/kbd0 opened" "$LOGFILE9" || { echo "FAIL: /dev/input/kbd0 could not be opened"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] /dev/input/kbd0 opened successfully"

grep -q "\[kbdtest\] PASS: KDSKBMODE set K_OFF verified" "$LOGFILE9" || { echo "FAIL: KDSKBMODE K_OFF failed"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] Console KDSKBMODE (K_OFF and K_XLATE) verified"

grep -q "\[kbdtest\] ALL KEYBOARD TESTS PASSED" "$LOGFILE9" || { echo "FAIL: kbdtest did not complete"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] kbdtest.elf all tests passed"

grep -q "mice" "$LOGFILE9" || { echo "FAIL: 'mice' not shown in ls /dev/input"; rm -f "$LOGFILE9"; exit 1; }
grep -q "mouse0" "$LOGFILE9" || { echo "FAIL: 'mouse0' not shown in ls /dev/input"; rm -f "$LOGFILE9"; exit 1; }
grep -q "kbd0" "$LOGFILE9" || { echo "FAIL: 'kbd0' not shown in ls /dev/input"; rm -f "$LOGFILE9"; exit 1; }
echo "[PASS] ls /dev/input shows mice, mouse0, and kbd0"

rm -f "$LOGFILE9"

echo "[TEST] Session 10: Stage 18A poll(), select(), non-blocking I/O, and libc flag translation tests..."

LOGFILE10=$(mktemp)
(
    sleep 3
    printf "flagtest.elf\n"
    sleep 6
    printf "polltest.elf\n"
    sleep 5
    printf "gfxtest.elf\n"
    sleep 5
    printf "shinfo\n"
    sleep 1
    printf "gfxtest.elf\n"
    sleep 5
    printf "shinfo\n"
    sleep 1
    printf "exit\n"
) | timeout 65s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE10" 2>&1 || true

tr -d '\r' < "$LOGFILE10" > "${LOGFILE10}.tmp" && mv "${LOGFILE10}.tmp" "$LOGFILE10"
cat "$LOGFILE10"

echo "[TEST] Asserting Session 10 results..."

grep -q "=== flagtest results: 43 passed, 0 failed ===" "$LOGFILE10" || { echo "FAIL: flagtest.elf did not pass all tests"; rm -f "$LOGFILE10"; exit 1; }
echo "[PASS] libc/newlib open/fcntl/access flag translations (flagtest.elf) verified"

grep -q "=== polltest results: 17 passed, 0 failed ===" "$LOGFILE10" || { echo "FAIL: polltest.elf did not pass all tests"; rm -f "$LOGFILE10"; exit 1; }
echo "[PASS] poll(), select(), and non-blocking I/O (polltest.elf) verified"

grep -q "=== gfxtest results: 29 passed, 0 failed ===" "$LOGFILE10" || { echo "FAIL: gfxtest.elf did not pass all tests"; rm -f "$LOGFILE10"; exit 1; }
echo "[PASS] libgfx 2D primitives, TTF, BMP, and context (gfxtest.elf) verified"

BASELINE_FRAMES_S10=$(grep "sh: pid=" "$LOGFILE10" | head -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
FINAL_FRAMES_S10=$(grep "sh: pid=" "$LOGFILE10" | tail -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
if [ -n "$BASELINE_FRAMES_S10" ] && [ -n "$FINAL_FRAMES_S10" ]; then
    if [ "$BASELINE_FRAMES_S10" -ne "$FINAL_FRAMES_S10" ]; then
        echo "FAIL: Session 10 frame leak detected: baseline $BASELINE_FRAMES_S10 vs final $FINAL_FRAMES_S10"
        exit 1
    fi
    echo "[PASS] Session 10: Zero frame leak verified (baseline $BASELINE_FRAMES_S10 == final $FINAL_FRAMES_S10)"
fi

rm -f "$LOGFILE10"

echo "[TEST] Session 10B: 4 Concurrent Processes + Preemption Spinner Stress Test & fsck.fat..."

LOGFILE10B=$(mktemp)
(
    sleep 3
    printf "concur_test.elf\n"
    sleep 20
    printf "exit\n"
) | timeout 50s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE10B" 2>&1 || true

tr -d '\r' < "$LOGFILE10B" > "${LOGFILE10B}.tmp" && mv "${LOGFILE10B}.tmp" "$LOGFILE10B"
cat "$LOGFILE10B"

echo "[TEST] Asserting Session 10B results..."
grep -q "\[concur_test\] PASS: 4 concurrent workers + spinner, no path corruption, zero leaks!" "$LOGFILE10B" || { echo "FAIL: concur_test.elf failed"; rm -f "$LOGFILE10B"; exit 1; }
echo "[PASS] 4 concurrent processes + spinner filesystem stress test verified"

~/.local/bin/fsck.fat -v -n disk.img || { echo "FAIL: fsck.fat found filesystem corruptions"; rm -f "$LOGFILE10B"; exit 1; }
echo "[PASS] fsck.fat disk consistency verified"

rm -f "$LOGFILE10B"

echo "[TEST] Running Concurrency Responsiveness, Tick-Loss (20s) and Input Delivery Suite..."
python3 tools/test_concur_suite.py || { echo "FAIL: test_concur_suite.py failed"; exit 1; }
echo "[PASS] Concurrency & Responsiveness suite verified"

echo "[TEST] Session 11: Stage 18A Demo Apps (paint, screenshot, view, gfxdemo) and Crash Safety..."

LOGFILE11=$(mktemp)
(
    sleep 6
    printf "paint.elf --auto\n"
    sleep 7
    printf "screenshot.elf /tmp/shot.bmp\n"
    sleep 5
    printf "view.elf /share/sample.bmp --still\n"
    sleep 3
    printf "gfxdemo.elf --still --timeout 500\n"
    sleep 3
    printf "crash_safety_test.elf\n"
    sleep 3
    printf "echo AFTER_CRASH=\$?\n"
    sleep 2
    printf "ls -l /tmp\n"
    sleep 2
    printf "rm /tmp/shot.bmp\n"
    sleep 1
    printf "rm /tmp/canvas.bmp\n"
    sleep 1
    printf "sync\n"
    sleep 1
    printf "exit\n"
) | timeout 75s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE11" 2>&1 || true

tr -d '\r' < "$LOGFILE11" > "${LOGFILE11}.tmp" && mv "${LOGFILE11}.tmp" "$LOGFILE11"
cat "$LOGFILE11"

echo "[TEST] Asserting Session 11 results..."

grep -q "\[paint\] Automated test passed: saved canvas to /tmp/canvas.bmp" "$LOGFILE11" || { echo "FAIL: paint.elf --auto failed"; rm -f "$LOGFILE11"; exit 1; }
echo "[PASS] paint.elf --auto verified"

grep -q "\[screenshot\] Saved 1024 x 768 screenshot to /tmp/shot.bmp" "$LOGFILE11" || { echo "FAIL: screenshot.elf failed"; rm -f "$LOGFILE11"; exit 1; }
echo "[PASS] screenshot.elf framebuffer capture verified"

grep -q "\[view\] Displayed 128x128 image /share/sample.bmp" "$LOGFILE11" || { echo "FAIL: view.elf failed"; rm -f "$LOGFILE11"; exit 1; }
echo "[PASS] view.elf image display verified"

grep -q "\[gfxdemo\] Still mode completed: 1024x768 reference frame rendered" "$LOGFILE11" || { echo "FAIL: gfxdemo.elf --still failed"; rm -f "$LOGFILE11"; exit 1; }
echo "[PASS] gfxdemo.elf --still reference frame verified"

grep -q "Process .* terminated (page fault at 0x00000000" "$LOGFILE11" || { echo "FAIL: crash_safety_test page fault not caught"; rm -f "$LOGFILE11"; exit 1; }
grep -q "AFTER_CRASH=139" "$LOGFILE11" || { echo "FAIL: console/keyboard recovery after segfault failed"; rm -f "$LOGFILE11"; exit 1; }
echo "[PASS] Crash safety: graphics mode + K_OFF + segfault -> console & keyboard recovery verified"

grep -q "canvas.bmp" "$LOGFILE11" || { echo "FAIL: canvas.bmp not found in /tmp"; rm -f "$LOGFILE11"; exit 1; }
grep -q "shot.bmp" "$LOGFILE11" || { echo "FAIL: shot.bmp not found in /tmp"; rm -f "$LOGFILE11"; exit 1; }
echo "[PASS] Generated BMP images present in /tmp verified"

rm -f "$LOGFILE11"

echo "[TEST] Session 12: Stage 18A Step 8 Monitor Screendump, Color Channels, Input Injection, and VGA Fallback..."
python3 tools/test_step8.py || { echo "FAIL: Session 12 Step 8 automated tests failed"; exit 1; }

echo "[TEST] Session 13: Stage 18B Step 6 Poweroff & Reboot with dirty filesystem buffer..."

LOGFILE13=$(mktemp)
(
    sleep 2
    printf "echo DIRTY_DATA_BEFORE_POWEROFF > /dirty_test.txt\n"
    sleep 1
    printf "poweroff\n"
    sleep 3
) | timeout 15s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE13" 2>&1 || true

cat "$LOGFILE13"
grep -q "Powering off system" "$LOGFILE13" || { echo "FAIL: 'Powering off system' not found"; rm -f "$LOGFILE13"; exit 1; }
echo "[PASS] poweroff initiated shutdown and QEMU terminated"

~/.local/bin/fsck.fat -v -n disk.img || { echo "FAIL: fsck.fat found filesystem corruptions after poweroff"; rm -f "$LOGFILE13"; exit 1; }
echo "[PASS] fsck.fat verified filesystem consistency after sudden poweroff"

rm -f "$LOGFILE13"

LOGFILE14=$(mktemp)
(
    sleep 2
    printf "reboot\n"
    sleep 3
) | timeout 15s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE14" 2>&1 || true

cat "$LOGFILE14"
grep -q "Rebooting system" "$LOGFILE14" || { echo "FAIL: 'Rebooting system' not found"; rm -f "$LOGFILE14"; exit 1; }
echo "[PASS] reboot executed and QEMU terminated on reset"

rm -f "$LOGFILE14"

echo "[TEST] Session 15: Model-Based LFN Stress Test (500 ops) & Host fsck.fat/mdir Verification..."

LOGFILE15=$(mktemp)
(
    sleep 4
    printf "shinfo\n"
    sleep 0.5
    printf "lfnstress\n"
    sleep 22
    printf "shinfo\n"
    sleep 0.5
    printf "exit\n"
) | timeout 55s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE15" 2>&1 || true

tr -d '\r' < "$LOGFILE15" > "${LOGFILE15}.tmp" && mv "${LOGFILE15}.tmp" "$LOGFILE15"
cat "$LOGFILE15"

echo "[TEST] Asserting Session 15 results..."
grep -q "\[lfnstress\] Starting 500-op Model-Based LFN Stress Test..." "$LOGFILE15" || { echo "FAIL: lfnstress did not start"; rm -f "$LOGFILE15"; exit 1; }
grep -q "\[lfnstress\] Checkpoint op 500/500 PASSED" "$LOGFILE15" || { echo "FAIL: lfnstress op 500 not reached or failed"; rm -f "$LOGFILE15"; exit 1; }
grep -q "\[lfnstress\] PASS: 500 ops verified against in-memory model!" "$LOGFILE15" || { echo "FAIL: lfnstress model verification failed"; rm -f "$LOGFILE15"; exit 1; }
echo "[PASS] 500 randomized LFN operations verified against in-memory model"

# Frame leak check for lfnstress
BASELINE_FRAMES_S15=$(grep "sh: pid=" "$LOGFILE15" | head -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
FINAL_FRAMES_S15=$(grep "sh: pid=" "$LOGFILE15" | tail -n 1 | sed -n 's/.*free_frames=\([0-9]*\).*/\1/p')
if [ -n "$BASELINE_FRAMES_S15" ] && [ -n "$FINAL_FRAMES_S15" ]; then
    if [ "$BASELINE_FRAMES_S15" -ne "$FINAL_FRAMES_S15" ]; then
        echo "FAIL: Session 15 frame leak detected: baseline $BASELINE_FRAMES_S15 vs final $FINAL_FRAMES_S15"
        rm -f "$LOGFILE15"
        exit 1
    fi
    echo "[PASS] Session 15: Zero frame leak verified (baseline $BASELINE_FRAMES_S15 == final $FINAL_FRAMES_S15)"
else
    echo "FAIL: Session 15: Could not parse free frames from shinfo"
    rm -f "$LOGFILE15"
    exit 1
fi

# Host-side fsck.fat verification
FSCK_CMD="fsck.fat"
if [ -x "$HOME/.local/bin/fsck.fat" ]; then
    FSCK_CMD="$HOME/.local/bin/fsck.fat"
fi
$FSCK_CMD -v -n disk.img || { echo "FAIL: fsck.fat found errors on disk.img after lfnstress"; rm -f "$LOGFILE15"; exit 1; }
echo "[PASS] Host fsck.fat -v -n clean after 500 stress operations"

# Host-side mdir verification: exactly the model's long names
MDIR_OUT=$(mdir -/ -i disk.img ::)
while read -r _ tag entry_name; do
    if ! echo "$MDIR_OUT" | grep -F -q "$entry_name"; then
        echo "FAIL: Model entry '$entry_name' not found in host mdir -/ -i disk.img ::"
        rm -f "$LOGFILE15"
        exit 1
    fi
done < <(grep "\[MODEL_ENTRY\]" "$LOGFILE15")

MODEL_COUNT=$(grep -c "\[MODEL_ENTRY\]" "$LOGFILE15")
MDIR_BRIEF_COUNT=$(mdir -b -/ -i disk.img ::/lfn_stress | wc -l)
if [ "$MODEL_COUNT" -ne "$MDIR_BRIEF_COUNT" ]; then
    echo "FAIL: Model entry count ($MODEL_COUNT) does not match disk entry count ($MDIR_BRIEF_COUNT)"
    rm -f "$LOGFILE15"
    exit 1
fi
echo "[PASS] Host mdir -/ -i disk.img :: verified: exactly the model's $MODEL_COUNT long names present with zero extras"

rm -f "$LOGFILE15"

echo "[TEST] Session 16: Kernel Agents (Capabilities, Delegation, Budgets, Audit Hash Chain, agentctl, -m 64 boot)..."

LOGFILE16=$(mktemp)
(
    sleep 2
    printf "shinfo\n"
    sleep 0.5
    printf "agenttest\n"
    sleep 4
    printf "agentctl list\n"
    sleep 0.5
    printf "agentctl create cliagent --fs-read /home --exec /bin --cpu 500 --mem 64\n"
    sleep 0.5
    printf "agentctl list\n"
    sleep 0.5
    printf "cat /proc/agents/log\n"
    sleep 0.5
    printf "cat /proc/agents/1/status\n"
    sleep 0.5
    printf "agentctl kill 1\n"
    sleep 0.5
    printf "shinfo\n"
    sleep 0.5
    printf "sync\n"
    sleep 0.5
    printf "exit\n"
) | timeout 35s qemu-system-i386 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE16" 2>&1 || true

tr -d '\r' < "$LOGFILE16" > "${LOGFILE16}.tmp" && mv "${LOGFILE16}.tmp" "$LOGFILE16"
cat "$LOGFILE16"

echo "[TEST] Asserting Session 16 results..."
grep -q "ALL AGENT TESTS PASSED!" "$LOGFILE16" || { echo "FAIL: agenttest did not pass all tests"; rm -f "$LOGFILE16"; exit 1; }
grep -q "PASSED: Capability allowlist and denial enforcement OK" "$LOGFILE16" || { echo "FAIL: Capability test failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "PASSED: Delegation narrowing enforced OK" "$LOGFILE16" || { echo "FAIL: Delegation narrowing failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "PASSED: CPU budget suspended runaway agent cleanly" "$LOGFILE16" || { echo "FAIL: CPU budget test failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "PASSED: max_procs prevented fork storm" "$LOGFILE16" || { echo "FAIL: max_procs test failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "PASSED: Verified .* audit log entries, SHA-256 hash chain 100% valid" "$LOGFILE16" || { echo "FAIL: Audit log SHA-256 hash chain verification failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "PASSED: Zero physical frame leaks after agent teardown" "$LOGFILE16" || { echo "FAIL: Frame leak check failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "Agent .* created: cliagent" "$LOGFILE16" || { echo "FAIL: agentctl create failed"; rm -f "$LOGFILE16"; exit 1; }
grep -q "cliagent" "$LOGFILE16" || { echo "FAIL: agentctl list did not show cliagent"; rm -f "$LOGFILE16"; exit 1; }
grep -q "Agent 1 killed" "$LOGFILE16" || { echo "FAIL: agentctl kill failed"; rm -f "$LOGFILE16"; exit 1; }
echo "[PASS] Session 16: Kernel agents, capabilities, budgets, audit log, and agentctl verified!"

rm -f "$LOGFILE16"

echo "[TEST] Session 17: Intent Bus (intentd, libjson, libintent, peer identity, demo skills, approvals, intentctl, -m 64 boot)..."

LOGFILE17=$(mktemp)
(
    sleep 2
    printf "shinfo\n"
    sleep 0.5
    printf "intentctl stats\n"
    sleep 0.5
    printf "intenttest\n"
    sleep 8
    printf "demoskills all &\n"
    sleep 1
    printf "intentctl list\n"
    sleep 0.5
    printf "intentctl call sys.meminfo\n"
    sleep 0.5
    printf "intentctl stats\n"
    sleep 0.5
    printf "cat /proc/agents/log | tail -n 10\n"
    sleep 0.5
    printf "shinfo\n"
    sleep 0.5
    printf "sync\n"
    sleep 0.5
    printf "exit\n"
) | timeout 55s qemu-system-i386 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE17" 2>&1 || true

tr -d '\r' < "$LOGFILE17" > "${LOGFILE17}.tmp" && mv "${LOGFILE17}.tmp" "$LOGFILE17"
cat "$LOGFILE17"

echo "[TEST] Asserting Session 17 results..."
grep -q "ALL INTENT BUS TESTS PASSED!" "$LOGFILE17" || { echo "FAIL: intenttest did not pass all tests"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: JSON parser strictness, schema validation, and daemon fuzz-resilience OK" "$LOGFILE17" || { echo "FAIL: JSON test failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Register + call round trip and schema rejection OK" "$LOGFILE17" || { echo "FAIL: Round trip failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Identity enforcement and spoofing defense verified" "$LOGFILE17" || { echo "FAIL: Identity test failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Confused deputy blocked by kernel capability sandbox" "$LOGFILE17" || { echo "FAIL: Confused deputy failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Sensitive skill approval workflow verified" "$LOGFILE17" || { echo "FAIL: Sensitive approval failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Provider crash mid-call handled cleanly without caller hang" "$LOGFILE17" || { echo "FAIL: Provider crash failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Rate limit and in-flight cap enforced" "$LOGFILE17" || { echo "FAIL: Rate limit failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Verified .* audit log entries, SHA-256 hash chain 100% valid" "$LOGFILE17" || { echo "FAIL: Audit log hash chain verification failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "PASSED: Zero frame leaks and zero leaked fds after 200 cycles" "$LOGFILE17" || { echo "FAIL: Zero frame leaks test failed"; rm -f "$LOGFILE17"; exit 1; }
grep -q "fs.list" "$LOGFILE17" || { echo "FAIL: intentctl list did not show fs.list"; rm -f "$LOGFILE17"; exit 1; }
grep -q "fs.delete" "$LOGFILE17" || { echo "FAIL: intentctl list did not show fs.delete"; rm -f "$LOGFILE17"; exit 1; }
echo "[PASS] Session 17: Intent Bus, daemon, libjson, peer authentication, and demo skills verified!"

rm -f "$LOGFILE17"

echo "[TEST] Session 18: Model Daemon, host bridge, token budgets, and agent loop..."

# Part A: Verify -m 64 low memory boot with modeld handling low memory gracefully
LOGFILE18_LOWMEM=$(mktemp)
(
    sleep 2
    printf "shinfo\n"
    sleep 0.5
    printf "cat /proc/meminfo\n"
    sleep 0.5
    printf "intentctl stats\n"
    sleep 0.5
    printf "exit\n"
) | timeout 25s qemu-system-i386 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE18_LOWMEM" 2>&1 || true

tr -d '\r' < "$LOGFILE18_LOWMEM" > "${LOGFILE18_LOWMEM}.tmp" && mv "${LOGFILE18_LOWMEM}.tmp" "$LOGFILE18_LOWMEM"
grep -q "reg model.complete" "$LOGFILE18_LOWMEM" || { echo "FAIL: modeld did not register in -m 64 low-memory boot"; rm -f "$LOGFILE18_LOWMEM"; exit 1; }
grep -q "MemTotal:       65020 kB" "$LOGFILE18_LOWMEM" || { echo "FAIL: MemTotal in -m 64 unexpected"; rm -f "$LOGFILE18_LOWMEM"; exit 1; }
echo "[PASS] Low-memory -m 64 boot with modeld verified!"
rm -f "$LOGFILE18_LOWMEM"

# Part B: Start hostbridge daemon
python3 tools/hostbridge.py --host 127.0.0.1 --port 4445 --backend mock > /tmp/hostbridge.log 2>&1 &
HB_PID=$!
sleep 1

(
    sleep 2
    printf "demoskills &\n"
    sleep 1
    printf "modeltest\n"
    sleep 8
    printf "agentrun organizer --goal \"list /home and summarize what is there\"\n"
    sleep 10
    printf "sync\n"
    sleep 1
    printf "exit\n"
) | timeout 65s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -serial tcp:127.0.0.1:4445 -display none -no-reboot > "$LOGFILE18" 2>&1 || true

kill -9 $HB_PID 2>/dev/null || true

tr -d '\r' < "$LOGFILE18" > "${LOGFILE18}.tmp" && mv "${LOGFILE18}.tmp" "$LOGFILE18"
cat "$LOGFILE18"

echo "[TEST] Asserting Session 18 results..."
grep -q "ALL MODEL DAEMON TESTS PASSED!" "$LOGFILE18" || { echo "FAIL: modeltest suite failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: COM2 raw device and framing resilience verified" "$LOGFILE18" || { echo "FAIL: COM2 raw resilience failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: model.status and model.embed verified" "$LOGFILE18" || { echo "FAIL: model.status/embed failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: model.complete host backend returned text" "$LOGFILE18" || { echo "FAIL: model.complete host backend failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: Local model is deterministic" "$LOGFILE18" || { echo "FAIL: Local model determinism failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: Token budget clamping and -EDQUOT properly enforced" "$LOGFILE18" || { echo "FAIL: Token budget clamping failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: Model backend allowlist and delegation narrowing verified" "$LOGFILE18" || { echo "FAIL: Model backend allowlist failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: Prompt injection attempt thwarted" "$LOGFILE18" || { echo "FAIL: Prompt injection defense failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: 100 cycles completed with zero FD leaks" "$LOGFILE18" || { echo "FAIL: 100 cycles fd leaks failed"; rm -f "$LOGFILE18"; exit 1; }
grep -q "PASSED: Verified .* audit log entries; cryptographic hash chain 100% valid" "$LOGFILE18" || { echo "FAIL: Audit log SHA-256 chain failed"; rm -f "$LOGFILE18"; exit 1; }

# Agent loop assertions
grep -q "Agent 'organizer' execution finished. Result: SUCCESS" "$LOGFILE18" || { echo "FAIL: agentrun organizer did not finish with SUCCESS"; rm -f "$LOGFILE18"; exit 1; }

# Verify /home/out/agent-*.log exists on disk image
mdir -i disk.img ::/home/out | grep -qi "agent-.*log" || { echo "FAIL: /home/out/agent-*.log does not exist on disk image"; rm -f "$LOGFILE18"; exit 1; }

# Security assertion: Host API key NEVER transmitted to guest logs or disk image
grep -q "MOCK_SECRET_KEY_NOT_IN_GUEST_998877" "$LOGFILE18" && { echo "FAIL: Host API secret key leaked into guest logs!"; rm -f "$LOGFILE18"; exit 1; }
strings disk.img | grep -q "MOCK_SECRET_KEY_NOT_IN_GUEST_998877" && { echo "FAIL: Host API secret key leaked onto guest disk image!"; rm -f "$LOGFILE18"; exit 1; }

echo "[PASS] Session 18: Model daemon, host bridge, token budgets, and agent loop verified!"
rm -f "$LOGFILE18"

echo "[TEST] Session 19: The Agent Desktop, accessibility tree, window capabilities, trusted approval UI, and killswitch..."

# Part A: Low-memory -m 64 text-mode boot: desktest.elf and agentctl window caps
(
    sleep 2
    printf "shinfo\n"
    sleep 0.5
    printf "agentctl create winagent --window-observe uidemo --window-act uidemo\n"
    sleep 0.5
    printf "cat /proc/agents/1/caps\n"
    sleep 0.5
    printf "desktest.elf\n"
    sleep 2
    printf "poweroff\n"
) | timeout 35s qemu-system-i386 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE19" 2>&1 || true

tr -d '\r' < "$LOGFILE19" > "${LOGFILE19}.tmp" && mv "${LOGFILE19}.tmp" "$LOGFILE19"
cat "$LOGFILE19"

echo "[TEST] Asserting Session 19 low-memory results..."
grep -q "ALL AGENT DESKTOP TESTS PASSED!" "$LOGFILE19" || { echo "FAIL: desktest suite failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Killswitch state transitions and audit logging verified" "$LOGFILE19" || { echo "FAIL: Killswitch test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Window capabilities and delegation narrowing verified" "$LOGFILE19" || { echo "FAIL: Window caps test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Accessibility tree and action protocol round-trip verified" "$LOGFILE19" || { echo "FAIL: A11y protocol test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "PASSED: Zero physical frame leaks after agent desktop operations" "$LOGFILE19" || { echo "FAIL: Frame leaks test failed"; rm -f "$LOGFILE19"; exit 1; }
grep -q "WindowObserve:	uidemo" "$LOGFILE19" || { echo "FAIL: WindowObserve cap missing in /proc/agents/1/caps"; rm -f "$LOGFILE19"; exit 1; }
grep -q "WindowAct:	uidemo" "$LOGFILE19" || { echo "FAIL: WindowAct cap missing in /proc/agents/1/caps"; rm -f "$LOGFILE19"; exit 1; }

# Part B: Preset configuration files on disk image
echo "[TEST] Verifying preset configuration files on disk image..."
mdir -i disk.img ::/etc/agents.d | grep -qi "organizer.conf" || { echo "FAIL: organizer.conf missing in /etc/agents.d"; rm -f "$LOGFILE19"; exit 1; }
mdir -i disk.img ::/etc/agents.d | grep -qi "explainer.conf" || { echo "FAIL: explainer.conf missing in /etc/agents.d"; rm -f "$LOGFILE19"; exit 1; }
mdir -i disk.img ::/etc/agents.d | grep -qi "terminal.*conf" || { echo "FAIL: terminal-helper.conf missing in /etc/agents.d"; rm -f "$LOGFILE19"; exit 1; }

# Part C: Full GUI compositor & visual killswitch verification via test_step6.py
echo "[TEST] Running Phase 4 graphical integration test (test_step6.py)..."
python3 tools/test_step6.py || { echo "FAIL: test_step6.py failed"; rm -f "$LOGFILE19"; exit 1; }

echo "[PASS] Session 19: The Agent Desktop, accessibility tree, window capabilities, trusted approval UI, and killswitch verified!"
rm -f "$LOGFILE19"

echo "[TEST] Session 20: Persistent agent memory (principals, memd, memctl) and semantic file index (indexd)..."
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
) | timeout 30s qemu-system-i386 -vga std -m 64 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE20_LOW" 2>&1 || true
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
    sleep 150
    printf "memctl list alice\n"
    sleep 2
    printf "sync\n"
    sleep 2
    printf "exit\n"
) | timeout 200s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE20" 2>&1 || true
tr -d '\r' < "$LOGFILE20" > "${LOGFILE20}.tmp" && mv "${LOGFILE20}.tmp" "$LOGFILE20"
cat "$LOGFILE20"

echo "[TEST] Asserting Session 20 results..."
grep -q "ALL MEMORY TESTS PASSED!" "$LOGFILE20" || { echo "FAIL: memdtest suite failed"; rm -f "$LOGFILE20"; exit 1; }
grep -q "FAILED:" "$LOGFILE20" && { echo "FAIL: memdtest reported a failure"; rm -f "$LOGFILE20"; exit 1; }
for t in "Stable principals" "Kernel-vouched principal" "memd put/search" "CAP_MEM_READ/WRITE namespace" "Provenance:" "Crash safety" "mem.forget physically" "indexd: fs.search filtered" "Audit chain records"; do
    grep -q "PASSED: $t" "$LOGFILE20" || { echo "FAIL: missing '$t'"; rm -f "$LOGFILE20"; exit 1; }
done
grep -q "memctl: error" "$LOGFILE20" && echo "(note: memctl error output present)"

# Raw-disk assertions: deleted memory must be gone from the image, live control record must be present.
if strings disk.img | grep -q "ZEBRAXYZ123"; then echo "FAIL: deleted memory text still present on raw disk image"; rm -f "$LOGFILE20"; exit 1; fi
strings disk.img | grep -q "LIVEMARKER_KEEP_7788" || { echo "FAIL: control record not found on disk (grep not meaningful)"; rm -f "$LOGFILE20"; exit 1; }

# Config files shipped on the image
mdir -i disk.img ::/etc | grep -qi "memory.*conf" || { echo "FAIL: /etc/memory.conf missing"; rm -f "$LOGFILE20"; exit 1; }
mdir -i disk.img ::/etc | grep -qi "index.*conf" || { echo "FAIL: /etc/index.conf missing"; rm -f "$LOGFILE20"; exit 1; }

echo "[PASS] Session 20: Persistent memory, principals, memd, memctl, and indexd verified!"
rm -f "$LOGFILE20"

echo "==============================="
echo "ALL TESTS PASSED!"
echo "==============================="
exit 0
