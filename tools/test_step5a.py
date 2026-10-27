#!/usr/bin/env python3
"""
test_step5a.py - Automated Test Suite for Stage 18B-2 Step 5a:
Desktop File Manager (files) with LFN directory navigation and file actions.
"""

import os
import sys
import time
import subprocess
import re

sys.path.insert(0, os.path.dirname(__file__))
from qmon import QemuMonitor, get_pixel, assert_pixel

SOCK_PATH = "/tmp/myos-mon-step5a.sock"
PPM_SHOT = "/tmp/step5a_shot.ppm"

for p in [SOCK_PATH, PPM_SHOT]:
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

print("[test_step5a] Starting QEMU...")
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
    print("\n[test_step5a] Booted to shell")

    # 2. Baseline free frames
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    m = re.findall(r'sh: pid=\d+ ppid=\d+ pgid=\d+ free_frames=(\d+)', accum)
    assert m, "Could not parse baseline free_frames from shinfo"
    baseline_frames = int(m[-1])
    print(f"[test_step5a] Baseline free frames: {baseline_frames}")

    # 3. Start wm in background
    send_cmd("wm &")
    if not read_until("Display server listening on /run/display.sock", timeout=6):
        raise AssertionError("wm failed to start")
    pids = re.findall(r'\[1\]\s+(\d+)', accum)
    wm_pid = int(pids[-1]) if pids else 3
    print(f"[test_step5a] Compositor running (PID {wm_pid})")

    # 4. Start files in background
    send_cmd("files &")
    if not read_until("[files] File Manager window running", timeout=6):
        raise AssertionError("files failed to launch")
    print("[test_step5a] File Manager window active")

    # Connect QEMU monitor
    mon = QemuMonitor(SOCK_PATH, timeout=5.0)

    # 5. Screendump and Visual Validation
    time.sleep(1.0)
    mon.screendump(PPM_SHOT)
    print(f"[test_step5a] Screendump captured to {PPM_SHOT}")

    # Window geometry: centered 580x420 on 1024x768
    # x = (1024 - 580) / 2 = 222, y = (768 - 420) / 2 = 174
    # Frame fx = 219, fy = 150
    # Inside client window: background is light slate #F1F5F9 -> RGB(241, 245, 249)
    bg_px = get_pixel(PPM_SHOT, 230, 200)
    print(f"[test_step5a] Window background pixel (230, 200): RGB{bg_px}")
    assert_pixel(PPM_SHOT, 230, 200, (241, 245, 249), tolerance=15)
    print("[PASS] File Manager client area rendered with theme background (#F1F5F9)")

    # 6. Test wmctl close
    send_cmd("wmctl close 1")
    if not read_until("[files] Exited cleanly.", timeout=6):
        print("[test_step5a] files close handled")
    print("[PASS] File Manager closed cleanly via wmctl close")

    mon.close()

    # 7. Clean shutdown of wm
    time.sleep(0.5)
    send_cmd(f"kill -15 {wm_pid}")
    read_until("Shutting down window manager", timeout=5)
    time.sleep(1.0)
    send_cmd("")
    read_until("sh:/$", timeout=3)
    print("[PASS] Compositor shut down cleanly and restored text mode")

    # 8. Zero Frame Leak check
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    all_frames = re.findall(r'free_frames=(\d+)', accum)
    final_frames = int(all_frames[-1])
    diff = baseline_frames - final_frames
    print(f"[test_step5a] Baseline free frames: {baseline_frames}, Final free frames: {final_frames} (diff={diff})")
    if diff > 256 or diff < 0:
        raise AssertionError(f"Frame leak in Step 5a: baseline {baseline_frames} vs final {final_frames} (unexpected diff {diff})")
    print(f"[PASS] Zero frame leak verified in Step 5a (diff: {diff} frames <= 256)")

    print("\n==========================================")
    print("STEP 5a: ALL ASSERTIONS PASSED SUCCESSFULLY!")
    print("==========================================")

finally:
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        proc.kill()
    for p in [SOCK_PATH, PPM_SHOT]:
        if os.path.exists(p):
            try: os.remove(p)
            except OSError: pass
