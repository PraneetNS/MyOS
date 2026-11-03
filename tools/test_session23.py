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
assert run_cmd("busybox sh -c 'test -f /hello.c && echo TEST_OK'", expect_pattern="TEST_OK", timeout=10)
assert run_cmd("busybox printf 'PRINTF_%s\\n' OK", expect_pattern="PRINTF_OK", timeout=10)
assert run_cmd("busybox sh -c 'true && echo TRUE_OK'", expect_pattern="TRUE_OK", timeout=10)

# 2b. BusyBox Deep Applet Coverage (sed, awk, sort, uniq, tr, cut, xargs, tar, gzip, find -exec, sh, diff, cp -r, ln -s, chmod, date)
assert run_cmd("busybox sed 's/disk/MyOS/' /hello.txt", expect_pattern="Hello from MyOS!", timeout=10)
assert run_cmd("busybox awk '{print $1}' /hello.txt", expect_pattern="Hello", timeout=10)
assert run_cmd("busybox sort /hello.txt", expect_pattern="Hello from disk!", timeout=10)
assert run_cmd("busybox uniq /hello.txt", expect_pattern="Hello from disk!", timeout=10)
assert run_cmd("busybox sh -c 'cat /hello.txt | busybox tr a-z A-Z'", expect_pattern="HELLO FROM DISK!", timeout=10)
assert run_cmd("busybox cut -d' ' -f1 /hello.txt", expect_pattern="Hello", timeout=10)
assert run_cmd("busybox sh -c 'echo \"ALPHA BETA\" | busybox xargs busybox echo XARGS_OUTPUT:'", expect_pattern="XARGS_OUTPUT: ALPHA BETA", timeout=10)
assert run_cmd("busybox sh -c 'for i in 1 2 3; do busybox echo \"ITEM_$i\"; done'", expect_pattern="ITEM_3", timeout=10)
assert run_cmd("busybox sh -c 'busybox cp -r /etc /tmp/etc_copy && busybox test -f /tmp/etc_copy/timezone && busybox echo CP_OK'", expect_pattern="CP_OK", timeout=10)
assert run_cmd("busybox sh -c 'busybox ln -sf /hello.txt /tmp/sym_hello.txt && busybox cat /tmp/sym_hello.txt'", expect_pattern="Hello from disk!", timeout=10)
assert run_cmd("busybox sh -c 'busybox diff -u /hello.txt /tmp/sym_hello.txt && busybox echo DIFF_OK'", expect_pattern="DIFF_OK", timeout=10)
assert run_cmd("busybox sh -c 'busybox chmod 0644 /tmp/sym_hello.txt && busybox echo CHMOD_OK'", expect_pattern="CHMOD_OK", timeout=10)
assert run_cmd("busybox date", expect_pattern="2026", timeout=10)
assert run_cmd("busybox sh -c 'busybox rm -f /tmp/test.tar /tmp/test.tar.gz && busybox tar -cf /tmp/test.tar /hello.txt && busybox test -f /tmp/test.tar && busybox echo TAR_OK'", expect_pattern="TAR_OK", timeout=10)
assert run_cmd("busybox sh -c 'busybox gzip /tmp/test.tar && busybox test -f /tmp/test.tar.gz && busybox echo GZIP_OK'", expect_pattern="GZIP_OK", timeout=10)
assert run_cmd("busybox sh -c 'busybox find / -name hello.txt -exec busybox echo FOUND:{} \";\"'", expect_pattern="FOUND:/hello.txt", timeout=10)

# 3. Lua 5.4.6 Musl
assert run_cmd("lua -e \"print('LUA_RESULT=' .. (6*7))\"", expect_pattern="LUA_RESULT=42", timeout=15)

# 4. MicroPython (Python)
assert run_cmd("python -c \"import sys; print('PYTHON_SYS_VERSION=' + sys.version)\"", expect_pattern="PYTHON_SYS_VERSION=3.4.0", timeout=15)

# 5. TCC Self-Hosting: Compile and Run
assert run_cmd("tcc -v", expect_pattern="tcc version 0.9.28rc", timeout=10)
assert run_cmd("tcc -B/usr/lib/tcc -B/usr/lib -static /hello.c -o /tmp/hello", timeout=20)
assert run_cmd("/tmp/hello", expect_pattern="HELLO FROM TCC SELF-HOSTING!", timeout=10)

# 6. Kernel Stack Guard Page & HWM Check
assert run_cmd("stacktest.elf", expect_pattern="PASS: child terminated with SIGSEGV", timeout=20)

# 7. Resource Leak Check (200 exec/exit, 200 open/symlink/unlink)
assert run_cmd("leaktest.elf", expect_pattern="PASS: 200 EXEC/EXIT AND 200 FS CYCLES COMPLETED WITH ZERO LEAKS", timeout=150)

# 8. Syscall Fuzzer & Sandbox Escape Auditor
assert run_cmd("sysfuzz.elf 1000", expect_pattern="PASS: SYSFUZZ PASSED WITH ZERO CRASHES, LEAKS, OR ESCAPES", timeout=90)

print("\n==========================================")
print("SESSION 23: ALL TESTS PASSED SUCCESSFULLY!")
print("==========================================")

proc.stdin.write(b"reboot\n")
proc.stdin.flush()
time.sleep(1)
proc.kill()
sys.exit(0)
