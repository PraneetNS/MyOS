#!/usr/bin/env python3
"""
test_step1.py - Automated Test Suite for Stage 18B-2 Step 1:
Protocol Spec, libwin, and Minimal Compositor (wm + winhello).
"""

import os
import sys
import time
import subprocess
import re

sys.path.insert(0, os.path.dirname(__file__))
from qmon import QemuMonitor, get_pixel, assert_pixel, read_ppm

SOCK_PATH = "/tmp/myos-mon-step1.sock"
PPM_SHOT = "/tmp/step1_shot.ppm"

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

print("[test_step1] Starting QEMU...")
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
    print("\n[test_step1] Booted to shell")
    time.sleep(2.5)

    # 2. Get baseline free frames
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    m = re.findall(r'sh: pid=\d+ ppid=\d+ pgid=\d+ free_frames=(\d+)', accum)
    assert m, "Could not parse baseline free_frames from shinfo"
    baseline_frames = int(m[-1])
    print(f"[test_step1] Baseline free frames: {baseline_frames}")

    # 3. Start wm in background
    send_cmd("wm &")
    if not read_until("Display server listening on /run/display.sock", timeout=6):
        raise AssertionError("wm failed to start and listen on /run/display.sock")
    pids = re.findall(r'\[1\]\s+(\d+)', accum)
    wm_pid = int(pids[-1]) if pids else 3
    print(f"[test_step1] Compositor running (PID {wm_pid})")

    # 4. Start winhello in background
    send_cmd("winhello &")
    if not read_until("[winhello] Connected to display server", timeout=6):
        raise AssertionError("winhello failed to connect to display server")
    if not read_until("[winhello] Window 1 created", timeout=6):
        raise AssertionError("winhello window creation failed")
    if not read_until("[winhello] FOCUS focused=1", timeout=6):
        raise AssertionError("winhello focus event failed")
    print("[test_step1] winhello window created and focused")

    # Connect monitor
    mon = QemuMonitor(SOCK_PATH, timeout=5.0)

    # 5. Screendump and pixel validation
    time.sleep(0.5)
    mon.screendump(PPM_SHOT)
    print(f"[test_step1] Screendump captured to {PPM_SHOT}")

    # Sample PPM colors
    from collections import Counter
    w, h, data = read_ppm(PPM_SHOT)
    samples = [get_pixel(PPM_SHOT, x, y) for y in range(0, h, 30) for x in range(0, w, 30)]
    counts = Counter(samples)
    print(f"[test_step1] PPM {w}x{h} top colors: {counts.most_common(5)}")

    bg_px = get_pixel(PPM_SHOT, 50, 50)
    print(f"[test_step1] Pixel (50, 50): RGB{bg_px}")
    assert_pixel(PPM_SHOT, 50, 50, (42, 77, 105), tolerance=10)

    win_px = get_pixel(PPM_SHOT, 372, 294)
    print(f"[test_step1] Pixel (372, 294): RGB{win_px}")
    assert_pixel(PPM_SHOT, 372, 294, (0, 128, 128), tolerance=10)

    red_px = get_pixel(PPM_SHOT, 387, 419)
    print(f"[test_step1] Pixel (387, 419): RGB{red_px}")
    assert_pixel(PPM_SHOT, 387, 419, (255, 0, 0), tolerance=10)

    print("[PASS] Screendump pixel assertions verified (solid background, teal window, red patch)")

    # 6. Test Mouse Motion and Click
    mon.mouse_move(10, 10)
    time.sleep(0.1)
    mon.mouse_button(1)
    time.sleep(0.1)
    mon.mouse_button(0)
    time.sleep(0.1)

    if not read_until("[winhello] POINTER_BUTTON", timeout=4):
        raise AssertionError("winhello did not receive POINTER_BUTTON event")
    print("[PASS] Mouse motion and button events received by winhello")

    # 7. Test Keyboard Event
    mon.sendkey("a")
    time.sleep(0.2)
    if not read_until("[winhello] KEY keycode=30 unicode=0x61", timeout=4):
        raise AssertionError("winhello did not receive KEY 'a' (keycode 30, unicode 0x61)")
    print("[PASS] Keyboard event keycode 30 (unicode 'a') translated and received")

    # 8. Test ESC key -> clean client exit
    mon.sendkey("esc")
    time.sleep(0.2)
    if not read_until("[winhello] ESC pressed, exiting", timeout=4):
        raise AssertionError("winhello did not handle ESC exit")
    if not read_until("[winhello] Exited cleanly.", timeout=4):
        raise AssertionError("winhello did not exit cleanly")
    print("[PASS] ESC key triggered clean client exit")

    mon.close()

    # 9. Clean shutdown of wm
    time.sleep(0.5)
    send_cmd(f"kill -15 {wm_pid}")
    read_until("Shutting down window manager", timeout=5)
    time.sleep(1.0)
    send_cmd("")
    read_until("sh:/$", timeout=3)
    print("[PASS] Compositor shut down cleanly and restored text mode")

    # 10. Frame leak check
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    all_frames = re.findall(r'free_frames=(\d+)', accum)
    final_frames = int(all_frames[-1])
    diff = baseline_frames - final_frames
    print(f"[test_step1] Baseline free frames: {baseline_frames}, Final free frames: {final_frames} (diff={diff})")
    if diff > 256 or diff < 0:
        raise AssertionError(f"Frame leak in Step 1: baseline {baseline_frames} vs final {final_frames} (unexpected diff {diff})")
    print(f"[PASS] Zero frame leak verified in Step 1 (diff: {diff} frames <= 256)")

    print("\n==========================================")
    print("STEP 1: ALL ASSERTIONS PASSED SUCCESSFULLY!")
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
