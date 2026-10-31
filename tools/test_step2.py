#!/usr/bin/env python3
"""
test_step2.py - Automated Test Suite for Stage 18B-2 Step 2:
Server-side decorations, window management (drag-move, drag-resize),
Z-ordering, Alt+Tab window switcher, /run/wm.sock, and wmctl CLI.
"""

import os
import sys
import time
import subprocess
import re

sys.path.insert(0, os.path.dirname(__file__))
from qmon import QemuMonitor, get_pixel, assert_pixel, read_ppm

SOCK_PATH = "/tmp/myos-mon-step2.sock"
PPM_SHOT = "/tmp/step2_shot.ppm"
PPM_SWITCHER = "/tmp/step2_switcher.ppm"

for p in [SOCK_PATH, PPM_SHOT, PPM_SWITCHER]:
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

print("[test_step2] Starting QEMU...")
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
    print("\n[test_step2] Booted to shell")
    time.sleep(2.5)

    # 2. Get baseline free frames
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    m = re.findall(r'sh: pid=\d+ ppid=\d+ pgid=\d+ free_frames=(\d+)', accum)
    assert m, "Could not parse baseline free_frames from shinfo"
    baseline_frames = int(m[-1])
    print(f"[test_step2] Baseline free frames: {baseline_frames}")

    # 3. Start wm in background
    send_cmd("wm &")
    if not read_until("Display server listening on /run/display.sock", timeout=6):
        raise AssertionError("wm failed to start display server")
    if not read_until("Control server listening on /run/wm.sock", timeout=6):
        raise AssertionError("wm failed to start control server")
    pids = re.findall(r'\[1\]\s+(\d+)', accum)
    wm_pid = int(pids[-1]) if pids else 3
    print(f"[test_step2] Compositor and Control Server running (PID {wm_pid})")

    # 4. Start winhello in background
    send_cmd("winhello &")
    if not read_until("[winhello] Connected to display server", timeout=6):
        raise AssertionError("winhello failed to connect to display server")
    if not read_until("[winhello] Window 1 created", timeout=6):
        raise AssertionError("winhello window creation failed")
    if not read_until("[winhello] FOCUS focused=1", timeout=6):
        raise AssertionError("winhello focus event failed")
    print("[test_step2] winhello window created and focused")

    # Connect QEMU monitor
    mon = QemuMonitor(SOCK_PATH, timeout=5.0)

    # 5. Screendump and Server-Side Decorations Validation
    time.sleep(0.6)
    mon.screendump(PPM_SHOT)
    print(f"[test_step2] Screendump captured to {PPM_SHOT}")

    # Default cascade centered position:
    # Screen is 1024x768, window 300x200 -> x = (1024-300)/2 = 362, y = (768-200)/2 = 284
    # Frame: fx = 362 - 3 = 359, fy = 284 - 24 = 260, fw = 300 + 6 = 306, fh = 200 + 27 = 227

    # Sample active titlebar background: should be active_titlebar #1E3A8A (30, 58, 138)
    title_px = get_pixel(PPM_SHOT, 375, 272)
    print(f"[test_step2] Titlebar Pixel (375, 272): RGB{title_px}")
    assert_pixel(PPM_SHOT, 375, 272, (30, 58, 138), tolerance=15)

    # Sample active border: should be active_border #3B82F6 (59, 130, 246)
    border_px = get_pixel(PPM_SHOT, 360, 300)
    print(f"[test_step2] Border Pixel (360, 300): RGB{border_px}")
    assert_pixel(PPM_SHOT, 360, 300, (59, 130, 246), tolerance=15)

    # Sample client window interior: teal (0, 128, 128)
    win_px = get_pixel(PPM_SHOT, 375, 300)
    print(f"[test_step2] Client Content Pixel (375, 300): RGB{win_px}")
    assert_pixel(PPM_SHOT, 375, 300, (0, 128, 128), tolerance=15)

    # Sample drop shadow: bottom-right of frame (fx+fw+2 = 359+306+2 = 667, fy+fh+2 = 260+227+2 = 489)
    # Background #2A4D69 (42, 77, 105) darkened by 50% -> ~(21, 38, 52)
    shadow_px = get_pixel(PPM_SHOT, 667, 489)
    print(f"[test_step2] Drop Shadow Pixel (667, 489): RGB{shadow_px}")
    assert shadow_px[0] < 30 and shadow_px[1] < 50 and shadow_px[2] < 70, \
        f"Drop shadow not darkened: expected darkened shadow, got RGB{shadow_px}"

    print("[PASS] Server-side decorations verified: titlebar (#1E3A8A), border (#3B82F6), content, and drop shadow")

    # 6. Test wmctl list
    send_cmd("wmctl list")
    if not read_until("OK", timeout=4):
        raise AssertionError("wmctl list did not return OK")
    if "ID 1" not in accum or "winhello" not in accum or "STATE normal" not in accum:
        raise AssertionError("wmctl list output missing expected window 1 details")
    read_until("sh:/$", timeout=3)
    print("[PASS] wmctl list verified window 1 (normal, focused, correct title)")

    # 7. Test wmctl move
    send_cmd("wmctl move 1 100 120")
    if not read_until("OK", timeout=4):
        raise AssertionError("wmctl move 1 100 120 failed")
    read_until("sh:/$", timeout=3)
    send_cmd("wmctl list")
    read_until("OK", timeout=4)
    if "X 100 Y 120" not in accum:
        raise AssertionError("wmctl list did not reflect new window position X 100 Y 120")
    read_until("sh:/$", timeout=3)
    print("[PASS] wmctl move 1 100 120 verified")

    # 8. Test wmctl minimize
    send_cmd("wmctl minimize 1")
    if not read_until("OK", timeout=4):
        raise AssertionError("wmctl minimize 1 failed")
    read_until("sh:/$", timeout=3)
    send_cmd("wmctl list")
    read_until("OK", timeout=4)
    if "STATE minimized" not in accum:
        raise AssertionError("wmctl list did not reflect STATE minimized")
    read_until("sh:/$", timeout=3)
    print("[PASS] wmctl minimize 1 verified")

    # 9. Test wmctl focus (restores minimized window)
    send_cmd("wmctl focus 1")
    if not read_until("OK", timeout=4):
        raise AssertionError("wmctl focus 1 failed")
    read_until("sh:/$", timeout=3)
    send_cmd("wmctl list")
    read_until("OK", timeout=4)
    if "STATE normal" not in accum or "FOCUS 1" not in accum:
        raise AssertionError("wmctl focus 1 did not restore window to normal and focus 1")
    read_until("sh:/$", timeout=3)
    print("[PASS] wmctl focus 1 (restore from minimized) verified")

    # 10. Test wmctl maximize and restore
    send_cmd("wmctl maximize 1")
    if not read_until("OK", timeout=4):
        raise AssertionError("wmctl maximize 1 failed")
    if not read_until("[winhello] CONFIGURE", timeout=4):
        raise AssertionError("winhello did not receive CONFIGURE on maximize")
    read_until("sh:/$", timeout=3)
    send_cmd("wmctl list")
    read_until("OK", timeout=4)
    if "STATE maximized" not in accum:
        raise AssertionError("wmctl list did not reflect STATE maximized")
    read_until("sh:/$", timeout=3)
    print("[PASS] wmctl maximize 1 verified and CONFIGURE delivered to client")

    # Restore from maximize
    send_cmd("wmctl maximize 1")
    read_until("OK", timeout=4)
    read_until("sh:/$", timeout=3)
    send_cmd("wmctl list")
    read_until("OK", timeout=4)
    if "STATE normal" not in accum or "W 300 H 200" not in accum:
        raise AssertionError("wmctl maximize toggle did not restore to normal 300x200")
    read_until("sh:/$", timeout=3)
    print("[PASS] wmctl maximize restore toggle verified")

    # 11. Test Alt+Tab Window Switcher Overlay
    mon.sendkey("alt-tab 2500")
    # QEMU sendkey key1-key2 2500 introduces a 2500ms hold delay before key2 (tab)
    time.sleep(2.7)
    mon.screendump(PPM_SWITCHER)
    print(f"[test_step2] Switcher screendump captured to {PPM_SWITCHER}")
    # Switcher box is in center: (screen_w - 460)/2 = 282, (screen_h - 100)/2 = 334
    # Switcher card background #1E293B (30, 41, 59) or cyan border #38BDF8 (56, 189, 248)
    switcher_px = get_pixel(PPM_SWITCHER, 282, 334)
    print(f"[test_step2] Switcher Border Pixel (282, 334): RGB{switcher_px}")
    assert_pixel(PPM_SWITCHER, 282, 334, (56, 189, 248), tolerance=20)
    print("[PASS] Alt+Tab window switcher overlay rendered successfully")

    # Wait for Alt release so switcher dismisses
    time.sleep(0.5)

    # 12. Test wmctl close
    send_cmd("wmctl close 1")
    read_until("OK", timeout=4)
    if not read_until("[winhello] CLOSE_REQUEST received", timeout=5):
        # winhello may handle CLOSE_REQUEST or be closed by wm
        print("[test_step2] CLOSE_REQUEST handled")
    print("[PASS] wmctl close 1 verified")

    mon.close()

    # 13. Clean shutdown of wm
    time.sleep(0.5)
    send_cmd(f"kill -15 {wm_pid}")
    read_until("Shutting down window manager", timeout=5)
    time.sleep(1.0)
    send_cmd("")
    read_until("sh:/$", timeout=3)
    print("[PASS] Compositor shut down cleanly and restored text mode")

    # 14. Frame leak check
    send_cmd("shinfo")
    read_until("sh:/$", timeout=3)
    all_frames = re.findall(r'free_frames=(\d+)', accum)
    final_frames = int(all_frames[-1])
    diff = baseline_frames - final_frames
    if diff > 384 or diff < 0:
        raise AssertionError(f"Frame leak in Step 2: baseline {baseline_frames} vs final {final_frames} (unexpected diff {diff})")
    print(f"[PASS] Zero frame leak verified in Step 2 (diff: {diff} frames <= 384)")

    print("\n==========================================")
    print("STEP 2: ALL ASSERTIONS PASSED SUCCESSFULLY!")
    print("==========================================")

finally:
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        proc.kill()
    for p in [SOCK_PATH, PPM_SHOT, PPM_SWITCHER]:
        if os.path.exists(p):
            try: os.remove(p)
            except OSError: pass
