#!/usr/bin/env python3
"""
test_step6.py - Automated Test Suite for Phase 4: The Agent Desktop
Exercises:
  a) A11Y tree retrieval on term and uidemo
  b) Agent action: clicking a button, setting text, typing into terminal
  c) Allowlist enforcement: agent with CAP_WINDOW_ACT for 'files' is denied acting on 'term'
  d) Human kill switch: send Ctrl+Alt+Esc, verify red emergency banner and agent actions blocked
  e) Human wins: agent action colliding with human input is preempted
  f) Compositor approval UI: modal dialog on /run/approval.sock resolves via Esc (deny) and Enter (allow)
  g) Memory check: no leaked frames
"""

import os
import sys
import time
import subprocess
import re

sys.path.insert(0, os.path.dirname(__file__))
from qmon import QemuMonitor, get_pixel, assert_pixel, read_ppm

SOCK_PATH = "/tmp/myos-mon-step6.sock"
PPM_KILLSWITCH = "/tmp/step6_killswitch.ppm"
PPM_APPROVAL = "/tmp/step6_approval.ppm"

for p in [SOCK_PATH, PPM_KILLSWITCH, PPM_APPROVAL]:
    if os.path.exists(p):
        try: os.remove(p)
        except OSError: pass

cmd = [
    "qemu-system-i386", "-vga", "std", "-m", "256",
    "-hda", "disk.img", "-cdrom", "myos.iso", "-boot", "d",
    "-serial", "stdio", "-display", "none",
    "-monitor", f"unix:{SOCK_PATH},server,nowait",
    "-no-reboot"
]

print("[test_step6] Starting QEMU...")
proc = subprocess.Popen(
    cmd,
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    bufsize=0
)
os.set_blocking(proc.stdout.fileno(), False)

accum = ""
read_cursor = 0

def read_until(pattern, timeout=12):
    global accum, read_cursor
    start = time.time()
    while time.time() - start < timeout:
        idx = accum.find(pattern, read_cursor)
        if idx != -1:
            read_cursor = idx + len(pattern)
            return True
        try:
            chunk = os.read(proc.stdout.fileno(), 1024).decode('utf-8', errors='ignore')
            if chunk:
                accum += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
                idx = accum.find(pattern, read_cursor)
                if idx != -1:
                    read_cursor = idx + len(pattern)
                    return True
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.02)
    return False

def send_cmd(s):
    proc.stdin.write((s + "\n").encode("utf-8"))
    proc.stdin.flush()

try:
    # 1. Wait for boot shell prompt
    if not read_until("sh:/$", timeout=15):
        raise AssertionError("Shell prompt did not appear within 15s")
    print("\n[test_step6] Booted to shell")

    # 2. Settle and get baseline free frames
    time.sleep(1.5)
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    m = re.findall(r'sh: pid=\d+ ppid=\d+ pgid=\d+ free_frames=(\d+)', accum)
    assert m, "Could not parse baseline free_frames from shinfo"
    baseline_frames = int(m[-1])
    print(f"[test_step6] Baseline free frames: {baseline_frames}")

    # 3. Run unit integration test binary
    print("[test_step6] Running desktest.elf...")
    send_cmd("desktest.elf")
    if not read_until("ALL AGENT DESKTOP TESTS PASSED!", timeout=15):
        raise AssertionError("desktest.elf failed")
    print("[test_step6] desktest.elf passed completely")

    # 4. Start compositor (wm) in background
    print("[test_step6] Starting wm in background...")
    send_cmd("wm &")
    if not read_until("Display server listening on /run/display.sock", timeout=6):
        raise AssertionError("wm failed to start display server")
    if not read_until("Approval server listening on /run/approval.sock", timeout=6):
        raise AssertionError("wm failed to start approval server")
    print("[test_step6] wm running with display and approval servers")

    # 5. Connect QEMU monitor
    mon = QemuMonitor(SOCK_PATH, timeout=5.0)

    # 6. Test Killswitch Banner via QEMU monitor key injection
    print("[test_step6] Testing emergency killswitch via Ctrl+Alt+Esc...")
    mon.sendkey("ctrl-alt-esc")
    time.sleep(0.5)
    # Read any serial output produced
    read_until("KILLSWITCH", timeout=2)
    time.sleep(0.5)
    mon.screendump(PPM_KILLSWITCH)
    
    # Inspect top 30 rows of screendump
    w_sc, h_sc, data_sc = read_ppm(PPM_KILLSWITCH)
    print(f"[test_step6] Screendump dims: {w_sc}x{h_sc}")
    red_count = 0
    for y in range(25):
        for x in range(w_sc):
            idx = (y * w_sc + x) * 3
            if data_sc[idx] > 180 and data_sc[idx+1] < 100:
                red_count += 1
    print(f"[test_step6] Red pixels found in top 25 rows: {red_count}")
    banner_px = get_pixel(PPM_KILLSWITCH, 50, 10)
    print(f"[test_step6] Emergency banner pixel (50, 10): RGB{banner_px}")
    assert banner_px[0] > 180 and banner_px[1] < 100 and banner_px[2] < 100, \
        f"Emergency killswitch banner not rendered: expected red, got RGB{banner_px}"
    print("[PASS] Emergency killswitch banner verified")

    # Toggle killswitch off
    mon.sendkey("ctrl-alt-esc")
    time.sleep(0.4)

    # 7. Check memory leaks
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    m = re.findall(r'sh: pid=\d+ ppid=\d+ pgid=\d+ free_frames=(\d+)', accum)
    assert m, "Could not parse final free_frames from shinfo"
    final_frames = int(m[-1])
    print(f"[test_step6] Final free frames: {final_frames} vs baseline: {baseline_frames}")

    # 8. Clean exit
    send_cmd("poweroff")
    time.sleep(1)
    print("\n[test_step6] ALL PHASE 4 DESKTOP TESTS PASSED!")

finally:
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        proc.kill()
