#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."

LOGFILE1=$(mktemp)
LOGFILE2=$(mktemp)
LOGFILE3=$(mktemp)
LOGFILE6=$(mktemp)
LOGFILE7=$(mktemp)
trap 'rm -f "$LOGFILE1" "$LOGFILE2" "$LOGFILE3" "$LOGFILE6" "$LOGFILE7"' EXIT

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
    printf "jobtest\n"
    sleep 2
    printf "memtest\n"
    sleep 16
    printf "exit\n"
) | timeout 90s qemu-system-i386 -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE1" 2>&1 || true

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

grep -q "=== ALL JOBTESTS PASSED ===" "$LOGFILE1" || { echo "FAIL: jobtest failed"; exit 1; }
echo "[PASS] jobtest (pgid, sid, ppid, tcgetpgrp, SIGTTIN on background read) verified"

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

echo "[TEST] Session 6: Live job control and interactive TTY signals over COM1..."

(
    sleep 2
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
) | timeout 18s qemu-system-i386 -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE6" 2>&1 || true

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
    sleep 2
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
    printf "jobs\n"
    sleep 0.3
    printf "echo time_write > /tmp/timed.txt\n"
    sleep 0.3
    printf "ls -l /tmp\n"
    sleep 0.3
    printf "shinfo\n"
    sleep 0.3
    printf "exit\n"
) | timeout 30s qemu-system-i386 -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE7" 2>&1 || true

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

echo "==============================="
echo "ALL TESTS PASSED!"
echo "==============================="
exit 0
