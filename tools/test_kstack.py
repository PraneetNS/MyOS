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
proc.stdin.write('stacktest.elf\n')
proc.stdin.flush()
time.sleep(3)
proc.stdin.write('cat /proc/self/status\n')
proc.stdin.flush()
time.sleep(2)
proc.stdin.write('reboot\n')
proc.stdin.flush()
try:
    out, _ = proc.communicate(timeout=8)
except Exception:
    proc.kill()
    out, _ = proc.communicate()
print(out)
