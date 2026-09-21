#!/bin/bash
# Builds every userland/*.c program into a standalone ELF32 executable.
# Each is compiled and linked completely separately from the kernel --
# real ELF output, not a flat binary -- and each is loaded at the same
# fixed virtual address (0x800000, see user.ld), which is safe now that
# Stage 6 gives every process its own page directory and fresh physical
# frames (see src/vmm.c).
set -e
cd "$(dirname "$0")"

for src in *.c; do
    name="${src%.c}"
    # NOTE: -O2 (and even -O1) silently miscompiled print_uint()'s
    # digit-writing loop in libc.h -- the store of each ASCII digit into
    # the buffer vanished entirely (confirmed by disassembly: the loop
    # computes the quotient via the usual multiply-by-magic-constant
    # trick but never writes a byte anywhere). This is exactly the kind
    # of thing worth watching for in a freestanding, no-libc environment:
    # the optimizer's assumptions are tuned for hosted code, and can go
    # wrong in ways that are silent, not a crash. -O0 sidesteps whatever
    # transformation causes it; these are small demo programs, so the
    # lost optimization doesn't matter.
    gcc -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
        -O0 -Wall -Wextra -nostdlib -c "$src" -o "$name.o"
    ld -m elf_i386 -T user.ld -nostdlib -o "$name.elf" "$name.o"
    echo "Built userland/$name.elf"
    readelf -h "$name.elf" 2>/dev/null | grep -E "Type|Entry|Machine" || true
done
