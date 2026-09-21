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
    gcc -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
        -O2 -Wall -Wextra -nostdlib -c "$src" -o "$name.o"
    ld -m elf_i386 -T user.ld -nostdlib -o "$name.elf" "$name.o"
    echo "Built userland/$name.elf"
    readelf -h "$name.elf" 2>/dev/null | grep -E "Type|Entry|Machine" || true
done
