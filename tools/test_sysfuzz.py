import subprocess, sys, time

cmd = [
    'qemu-system-x86_64', '-m', '256',
    '-cdrom', 'myos.iso',
    '-drive', 'file=disk.img,format=raw,index=0,media=disk',
    '-boot', 'd',
    '-serial', 'stdio',
    '-display', 'none',
    '-no-reboot'
]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(5)
proc.stdin.write('sysfuzz.elf 1000\n')
proc.stdin.flush()
time.sleep(20)
proc.stdin.write('reboot\n')
proc.stdin.flush()
try:
    out, _ = proc.communicate(timeout=10)
except Exception:
    proc.kill()
    out, _ = proc.communicate()

print(out)
if "PASS: SYSFUZZ PASSED WITH ZERO CRASHES, LEAKS, OR ESCAPES" in out:
    print("SYSFUZZ_OK")
    sys.exit(0)
else:
    print("SYSFUZZ_FAILED")
    sys.exit(1)
