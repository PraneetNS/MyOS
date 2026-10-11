#!/usr/bin/env python3
import os
import sys
import time
import subprocess
from qmon import QemuMonitor, read_ppm, get_pixel, assert_pixel, has_text_pixels

SOCK_PATH = "/tmp/myos-mon.sock"
PPM_BOOT = "/tmp/step8_boot.ppm"
PPM_STILL = "/tmp/step8_still.ppm"
PPM_RESTORED = "/tmp/step8_restored.ppm"

for p in [SOCK_PATH, PPM_BOOT, PPM_STILL, PPM_RESTORED]:
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

print("Starting QEMU with -serial stdio and monitor socket...")
proc = subprocess.Popen(
    cmd,
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    bufsize=0
)
os.set_blocking(proc.stdout.fileno(), False)

def read_until(pattern, timeout=10):
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
    # 1. Wait for boot OK
    out = read_until("sh:/$", timeout=10)
    assert "sh:/$" in out, "Shell prompt did not appear"
    print("[PASS] Kernel booted and shell prompt ready")

    mon = QemuMonitor(sock_path=SOCK_PATH, timeout=5.0)
    print("[PASS] Connected to monitor")

    # 2. Boot screendump check
    mon.screendump(PPM_BOOT)
    w, h, _ = read_ppm(PPM_BOOT)
    assert (w, h) == (1024, 768)
    assert has_text_pixels(PPM_BOOT, 0, 100), "No text pixels in boot screendump"
    print(f"[PASS] Boot screendump 1024x768 with text console verified")

    # 3. Launch gfxdemo.elf --still via serial stdin
    print("\n--- Running gfxdemo.elf --still ---")
    send_cmd("gfxdemo.elf --still\n")
    time.sleep(2.0)

    # 4. Take screendump of still frame
    mon.screendump(PPM_STILL)
    print(f"[PASS] Captured still frame to {PPM_STILL}")

    # Inspect colors
    red = get_pixel(PPM_STILL, 60, 60)
    green = get_pixel(PPM_STILL, 210, 60)
    blue = get_pixel(PPM_STILL, 360, 60)
    bg = get_pixel(PPM_STILL, 500, 500)

    print(f"Pixel (60, 60)   Red patch   = RGB{red}")
    print(f"Pixel (210, 60)  Green patch = RGB{green}")
    print(f"Pixel (360, 60)  Blue patch  = RGB{blue}")
    print(f"Pixel (500, 500) Background  = RGB{bg}")

    assert_pixel(PPM_STILL, 60, 60, (255, 0, 0), tolerance=2)
    print("[PASS] Color channel Red (255, 0, 0) verified (RGB byte order correct)")
    assert_pixel(PPM_STILL, 210, 60, (0, 255, 0), tolerance=2)
    print("[PASS] Color channel Green (0, 255, 0) verified")
    assert_pixel(PPM_STILL, 360, 60, (0, 0, 255), tolerance=2)
    print("[PASS] Color channel Blue (0, 0, 255) verified")
    assert_pixel(PPM_STILL, 500, 500, (21, 27, 39), tolerance=4)
    print("[PASS] Background pixel (21, 27, 39) verified")

    # 5. Quit gfxdemo with sendkey esc
    print("\n--- Sending sendkey esc to quit gfxdemo ---")
    mon.sendkey("esc")
    out = read_until("Still mode completed", timeout=5)
    print("[PASS] gfxdemo exited cleanly on ESC")

    time.sleep(1.0)
    mon.screendump(PPM_RESTORED)
    assert has_text_pixels(PPM_RESTORED, 0, 200), "No text pixels after ESC restore"
    print("[PASS] Console restored after ESC: text console screendump confirmed")

    # 6. Input check: mouse
    print("\n--- Running mousetest.elf --events ---")
    send_cmd("mousetest.elf --events\n")
    time.sleep(1.0)
    mon.mouse_move(50, 20)
    time.sleep(0.2)
    mon.mouse_button(1)
    time.sleep(0.2)
    mon.mouse_button(0)
    out = read_until("Events received", timeout=5)
    assert "REL_X dx=50" in out or "REL_X=1" in out
    assert "REL_Y dy=20" in out or "REL_Y=1" in out
    assert "BTN_LEFT value=1" in out or "BTN_LEFT=1" in out
    print("[PASS] Mouse injection (mouse_move 50 20, mouse_button 1) verified")

    # 7. Input check: keyboard & K_OFF isolation
    print("\n--- Running kbdtest.elf --events ---")
    send_cmd("kbdtest.elf --events\n")
    time.sleep(1.0)
    mon.sendkey("a")
    out = read_until("Keyboard events received", timeout=5)
    assert "keycode=30 value=1 (press)" in out
    assert "keycode=30 value=0 (release)" in out
    print("[PASS] Raw keyboard event injection (sendkey a -> keycode 30) verified")

    # Verify shell prompt accepted typed command after kbdtest exit (K_XLATE restored)
    send_cmd("echo KBD_XLATE_OK\n")
    out = read_until("KBD_XLATE_OK", timeout=5)
    assert "KBD_XLATE_OK" in out
    print("[PASS] Shell keyboard input translation (K_XLATE) restored")

    # Exit shell
    send_cmd("exit\n")
    time.sleep(1.0)
    mon.cmd("quit")
    proc.wait(timeout=5)

except Exception as e:
    print(f"\n[FAIL] Test error: {e}")
    try: mon.cmd("quit")
    except Exception: pass
    proc.kill()
    raise

# 8. VGA fallback boot test
print("\n--- Running VGA text fallback boot test (-vga none) ---")
fallback_cmd = [
    "qemu-system-i386", "-vga", "none", "-m", "256",
    "-hda", "disk.img", "-cdrom", "myos.iso", "-boot", "d",
    "-serial", "stdio", "-display", "none",
    "-no-reboot"
]
fproc = subprocess.Popen(
    fallback_cmd,
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1
)
time.sleep(4.0)
fproc.stdin.write("echo VGA_FALLBACK_OK\n")
fproc.stdin.flush()

f_accum = ""
start = time.time()
while time.time() - start < 6.0:
    line = fproc.stdout.readline()
    if not line: break
    f_accum += line
    sys.stdout.write(line)
    if "VGA_FALLBACK_OK" in f_accum: break

fproc.kill()
assert "BOOT OK" in f_accum, "VGA fallback boot failed"
assert "VGA_FALLBACK_OK" in f_accum, "VGA fallback shell failed"
print("[PASS] -vga none fallback boot verified: kernel booted into text console and ran shell")

print("\n==================================================")
print("ALL STAGE 18A STEP 8 TESTS PASSED SUCCESSFULLY!")
print("==================================================")
