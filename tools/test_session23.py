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

def read_until(pattern, timeout=15):
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

def run_cmd(command, expect_pattern=None, timeout=15):
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

if not read_until("sh:/$", timeout=25):
    print("FAILED TO BOOT TO SHELL")
    proc.kill()
    sys.exit(1)

print("\n--- SHELL READY ---")

# Wait a second for background daemons to settle
time.sleep(1)

# 1. Musl Libc Regression Suite
assert run_cmd("musltest.elf", expect_pattern="ALL MUSL TESTS PASSED", timeout=20)

# 2. BusyBox Applets Verification
assert run_cmd("busybox --help", expect_pattern="Currently defined functions:", timeout=15)
assert run_cmd("busybox echo HELLO_FROM_BUSYBOX", expect_pattern="HELLO_FROM_BUSYBOX", timeout=10)
assert run_cmd("busybox uname -a", expect_pattern="6.1.0-myos", timeout=10)
assert run_cmd("busybox cat /hello.txt", expect_pattern="Hello from disk!", timeout=10)
assert run_cmd("busybox grep disk /hello.txt", expect_pattern="disk", timeout=10)
assert run_cmd("busybox head -n 1 /hello.txt", expect_pattern="Hello from disk!", timeout=10)
assert run_cmd("busybox tail -n 2 /hello.txt", expect_pattern="executed the same way.", timeout=10)
assert run_cmd("busybox wc -l /hello.txt", expect_pattern="10", timeout=10)
assert run_cmd("busybox find / -name hello.c", expect_pattern="/hello.c", timeout=10)
assert run_cmd("busybox test -f /hello.c && busybox echo TEST_OK", expect_pattern="TEST_OK", timeout=10)
assert run_cmd("busybox printf 'PRINTF_%s\\n' OK", expect_pattern="PRINTF_OK", timeout=10)
assert run_cmd("busybox true && busybox echo TRUE_OK", expect_pattern="TRUE_OK", timeout=10)

# 3. Lua 5.4.6 Musl
assert run_cmd("lua -e \"print('LUA_RESULT=' .. (6*7))\"", expect_pattern="LUA_RESULT=42", timeout=15)

# 4. MicroPython (Python)
assert run_cmd("python -c \"import sys; print('PYTHON_SYS_VERSION=' + sys.version)\"", expect_pattern="PYTHON_SYS_VERSION=3.4.0", timeout=15)

# 5. TCC Self-Hosting: Compile and Run
assert run_cmd("tcc -v", expect_pattern="tcc version 0.9.28rc", timeout=10)
assert run_cmd("tcc -B/usr/lib/tcc -B/usr/lib -static /hello.c -o /tmp/hello", timeout=20)
assert run_cmd("/tmp/hello", expect_pattern="HELLO FROM TCC SELF-HOSTING!", timeout=10)

print("\n==========================================")
print("SESSION 23: ALL TESTS PASSED SUCCESSFULLY!")
print("==========================================")

proc.stdin.write(b"reboot\n")
proc.stdin.flush()
time.sleep(1)
proc.kill()
sys.exit(0)
