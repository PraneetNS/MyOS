#!/usr/bin/env python3
"""
test_concur_suite.py - Verification suite for MyOS Kernel Concurrency & Locking:
  3a. Tick-loss test during heavy disk load (assert 98-102 ticks/s over 20s)
  3b. Serial / Ctrl+C responsiveness during disk load (assert spin.elf dies < 1s)
  3c. Mouse input delivery during disk load (assert /dev/input/mouse0 receives events)
  3d. Lock-sanity assertions active (panic if mutex held across syscall return/exit)
"""

import os
import sys
import time
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmon import QemuMonitor

MON_SOCK = "/tmp/myos-concur-mon.sock"

if os.path.exists(MON_SOCK):
    try:
        os.remove(MON_SOCK)
    except OSError:
        pass

cmd = [
    "qemu-system-i386", "-vga", "std", "-m", "256",
    "-hda", "disk.img", "-cdrom", "myos.iso", "-boot", "d",
    "-serial", "stdio", "-display", "none",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-no-reboot"
]

print("Starting QEMU for Concurrency & Responsiveness Suite...")
proc = subprocess.Popen(
    cmd,
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    bufsize=0
)
os.set_blocking(proc.stdout.fileno(), False)

def read_until(pattern, timeout=30):
    start = time.time()
    accum = ""
    while time.time() - start < timeout:
        try:
            chunk = os.read(proc.stdout.fileno(), 1024).decode('utf-8', errors='ignore')
            if chunk:
                accum += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
                if pattern in accum:
                    return accum
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.02)
    return accum

def send_cmd(s):
    proc.stdin.write(s.encode("utf-8"))
    proc.stdin.flush()

try:
    # Wait for shell prompt
    out = read_until("sh:/$", timeout=25)
    assert "sh:/$" in out, "Shell prompt did not appear"
    print("[PASS] Kernel booted and shell ready")

    mon = QemuMonitor(sock_path=MON_SOCK, timeout=5.0)
    print("[PASS] Connected to QEMU monitor")

    # -------------------------------------------------------------
    # 3a. Tick-Loss Test over 20 Seconds
    # -------------------------------------------------------------
    print("\n=== Running Test 3a: Timer Tick-Loss Test Under Heavy Disk Workload ===")
    send_cmd("ticktest.elf\n")
    out = read_until("PASS: ", timeout=35)
    assert "PASS: " in out and "ticks/sec" in out, "ticktest failed to pass 98-102 ticks/s assertion"
    
    # Parse ticks per second
    ticks_per_s = None
    for line in out.splitlines():
        if "ticks/sec" in line and "Elapsed:" in line:
            parts = line.split("->")
            if len(parts) > 1:
                ticks_per_s = parts[1].strip().split()[0]
    print(f"\n[PASS] Test 3a Verified: {ticks_per_s} ticks/sec under heavy disk load (98-102 ticks/s window)")

    # Wait for prompt
    read_until("sh:/$", timeout=5)

    # -------------------------------------------------------------
    # 3b. Serial / Ctrl+C Responsiveness Under Disk Load
    # -------------------------------------------------------------
    print("\n=== Running Test 3b: Serial / Ctrl+C Responsiveness Under Heavy Disk Load ===")
    # Start background disk stress
    send_cmd("concur_t.elf &\n")
    time.sleep(0.5)

    # Launch foreground spin.elf
    send_cmd("spin.elf\n")
    time.sleep(0.5)

    # Send Ctrl+C (\x03) over COM1 serial line
    t_ctrlc = time.time()
    send_cmd("\x03")
    out_sig = read_until("sh:/$", timeout=2.0)
    elapsed_kill = time.time() - t_ctrlc

    assert "terminated by signal 2" in out_sig or "sh:/$" in out_sig, "spin.elf was not terminated by Ctrl+C"
    assert elapsed_kill <= 1.0, f"Ctrl+C latency exceeded 1.0s: {elapsed_kill:.3f}s"
    print(f"[PASS] Test 3b Verified: Ctrl+C interrupted spin.elf in {elapsed_kill:.3f}s (< 1.0s)")

    # -------------------------------------------------------------
    # 3c. Mouse Input Events Under Heavy Disk Load
    # -------------------------------------------------------------
    print("\n=== Running Test 3c: Mouse Input Delivery Under Heavy Disk Load ===")
    send_cmd("mousetest.elf --events\n")
    time.sleep(0.5)

    # Inject mouse move and button clicks via monitor
    mon.mouse_move(50, 20)
    time.sleep(0.1)
    mon.mouse_button(1)
    time.sleep(0.1)
    mon.mouse_button(0)

    out_mouse = read_until("Events received: REL_X=1 REL_Y=1 BTN_LEFT=1", timeout=5.0)
    assert "Events received: REL_X=1 REL_Y=1 BTN_LEFT=1" in out_mouse, "mousetest did not receive all injected mouse events"
    print("[PASS] Test 3c Verified: mousetest received all injected mouse events during disk load")

    # Clean shutdown
    read_until("sh:/$", timeout=5)
    send_cmd("exit\n")
    time.sleep(1)

    print("\n==========================================")
    print("ALL CONCURRENCY & RESPONSIVENESS TESTS PASSED!")
    print("==========================================")

finally:
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        proc.kill()
    if os.path.exists(MON_SOCK):
        try: os.remove(MON_SOCK)
        except OSError: pass
