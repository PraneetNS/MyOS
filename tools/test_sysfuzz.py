import subprocess, sys, time, argparse

parser = argparse.ArgumentParser(description="MyOS Sysfuzz Test Runner")
parser.add_argument("--iterations", type=int, default=1000, help="Number of fuzz iterations")
parser.add_argument("--seed", type=str, default="0x12345678", help="PRNG seed")
parser.add_argument("--timeout", type=int, default=45, help="Fuzz timeout in seconds")
args = parser.parse_args()

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
proc.stdin.write(f'sysfuzz.elf --iterations {args.iterations} --seed {args.seed}\n')
proc.stdin.flush()
time.sleep(args.timeout)
proc.stdin.write('reboot\n')
proc.stdin.flush()
try:
    out, _ = proc.communicate(timeout=15)
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
