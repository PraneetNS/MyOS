#!/bin/bash
# Builds every userland/*.c program using tools/myos-gcc and Newlib libc.
set -e
cd "$(dirname "$0")"

MYOS_GCC="../tools/myos-gcc"
if [ ! -x "$MYOS_GCC" ]; then
    echo "Error: $MYOS_GCC not found or not executable" >&2
    exit 1
fi

make -C ../libmyos all

for src in *.c; do
    name="${src%.c}"
    "$MYOS_GCC" "$src" -o "$name.elf"
    echo "Built userland/$name.elf"
    readelf -h "$name.elf" 2>/dev/null | grep -E "Type|Entry|Machine" || true
done
