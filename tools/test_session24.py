import os, sys, time, subprocess

cmd = [
    "qemu-system-x86_64", "-vga", "std", "-m", "256",
    "-hda", "disk.img", "-cdrom", "myos.iso", "-boot", "d",
    "-serial", "stdio", "-display", "none",
    "-no-reboot"
]

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

def read_until(pattern, timeout=25):
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
        except (BlockingIOError, InterruptedError):
            pass
        time.sleep(0.05)
    return False

def run_cmd(command, expect_pattern=None, timeout=25):
    print(f"\n==========================================")
    print(f">>> RUNNING: {command}")
    print(f"==========================================")
    proc.stdin.write(f"{command}\n".encode('utf-8'))
    proc.stdin.flush()
    if expect_pattern:
        ok = read_until(expect_pattern, timeout=timeout)
        if not ok:
            print(f"FAILED waiting for expected pattern '{expect_pattern}' after running '{command}'")
            return False
    ok_prompt = read_until("sh:/$", timeout=timeout)
    if not ok_prompt:
        print(f"FAILED waiting for prompt after running '{command}'")
        return False
    return True

if not read_until("sh:/$", timeout=30):
    print("FAILED TO BOOT TO SHELL")
    proc.kill()
    sys.exit(1)

print("\n--- SHELL READY ---")
time.sleep(1)

# Session 24: POSIX Threads & Futex Validation
assert run_cmd("threadtest.elf", expect_pattern="ALL THREAD TESTS PASSED (8/8)", timeout=35)

print("\n==========================================")
print("SUCCESS: Session 24 POSIX Threads & Futex suite passed!")
print("==========================================")
proc.stdin.write(b"poweroff\n")
proc.stdin.flush()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
sys.exit(0)
