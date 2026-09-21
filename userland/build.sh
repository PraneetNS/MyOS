#!/bin/bash
# Builds userland/hello.c into a standalone ELF32 executable (hello.elf),
# completely separately from the kernel build. This is real ELF output --
# not a flat binary -- so the kernel's elf.c has genuine program headers
# to parse.
set -e
cd "$(dirname "$0")"

gcc -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
    -O2 -Wall -Wextra -nostdlib -c hello.c -o hello.o

ld -m elf_i386 -T user.ld -nostdlib -o hello.elf hello.o

echo "Built userland/hello.elf:"
file hello.elf 2>/dev/null || true
readelf -h hello.elf 2>/dev/null | grep -E "Type|Entry|Machine" || true
