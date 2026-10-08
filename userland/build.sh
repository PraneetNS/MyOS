#!/bin/bash
# Builds every userland/*.c program using tools/myos-gcc and Newlib libc.
set -e
cd "$(dirname "$0")"

MYOS_GCC="../tools/myos-gcc"
PREFIX="${MYOS_PREFIX:-$HOME/opt/myos-cross}"
STRIP="$PREFIX/bin/i686-elf-strip"

if [ ! -x "$MYOS_GCC" ]; then
    echo "Error: $MYOS_GCC not found or not executable" >&2
    exit 1
fi

make -C ../libmyos all

for src in *.c; do
    name="${src%.c}"
    "$MYOS_GCC" -O2 "$src" -o "$name.elf"
    echo "Built userland/$name.elf"
done

if [ -f "kilo.elf" ]; then
    cp kilo.elf edit.elf
fi

# Build Lua if present
if [ -d "lua/src" ]; then
    ROOT_DIR="$(cd .. && pwd)"
    make -C lua/src posix CC="$ROOT_DIR/tools/myos-gcc" AR="$PREFIX/bin/i686-elf-ar rcu" RANLIB="$PREFIX/bin/i686-elf-ranlib"
    cp lua/src/lua lua.elf
    cp lua/src/luac luac.elf
fi

# Strip all ELFs to minimize disk footprint
for elf in *.elf; do
    if [ -f "$elf" ] && [ -x "$STRIP" ]; then
        "$STRIP" -s "$elf" || true
    fi
done
