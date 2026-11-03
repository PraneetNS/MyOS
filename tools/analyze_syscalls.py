import sys

# 1. Parse libmyos/include/myos_syscalls.h
defs = {}
with open("libmyos/include/myos_syscalls.h", "r") as f:
    for line in f:
        line = line.strip()
        if line.startswith("#define "):
            parts = line[8:].split()
            if len(parts) >= 2:
                name = parts[0]
                val = parts[1]
                defs[name] = val

# Resolve recursive defs (e.g. SYS_READ -> __NR_read -> 0)
def resolve_val(name):
    visited = set()
    cur = name
    while cur in defs and cur not in visited:
        visited.add(cur)
        val = defs[cur]
        if val.isdigit():
            return int(val)
        cur = val
    if cur.isdigit():
        return int(cur)
    return None

# Also add direct numeric cases
with open("src/syscall.c", "r") as f:
    lines = f.readlines()

handled_nums = {}
for idx, line in enumerate(lines):
    line_clean = line.strip()
    if line_clean.startswith("case "):
        rest = line_clean[5:].strip()
        colon = rest.find(":")
        if colon != -1:
            val = rest[:colon].strip()
            num = None
            if val.isdigit():
                num = int(val)
            else:
                num = resolve_val(val)
            if num is not None:
                handled_nums[num] = (val, idx + 1)
            else:
                print(f"UNRESOLVED: {val} at line {idx+1}")

print(f"Total unique syscall numbers handled: {len(handled_nums)}")
linux_handled = {k: v for k, v in handled_nums.items() if k < 1000}
myos_handled = {k: v for k, v in handled_nums.items() if k >= 1000}
print(f"Linux (<1000) syscalls handled: {len(linux_handled)}")
print(f"MyOS (>=1000) syscalls handled: {len(myos_handled)}")

# Pinned Linux kernel version: Linux 6.1 LTS
# Linux 6.1 has syscalls 0 to 450 (451 syscalls total)
print(f"Linux 6.1 x86-64 total syscalls: 451 (0..450)")
print(f"Implemented/stubbed in MyOS: {len(linux_handled)} / 451 = {len(linux_handled)/451*100:.1f}%")
print(f"Returning -ENOSYS: {451 - len(linux_handled)} / 451 = {(451 - len(linux_handled))/451*100:.1f}%")

print("\nHandled Linux syscall numbers:")
for nr in sorted(linux_handled.keys()):
    print(f"  {nr:3d}: {linux_handled[nr][0]}")

print("\nHandled MyOS syscall numbers:")
for nr in sorted(myos_handled.keys()):
    print(f"  {nr:4d}: {myos_handled[nr][0]}")
