#!/bin/bash
# Builds every userland/*.c program using tools/myos-gcc and Newlib libc.
set -e
cd "$(dirname "$0")"

MYOS_GCC="../tools/myos-gcc"
PREFIX="${MYOS_PREFIX:-$HOME/opt/myos-cross}"
STRIP="$PREFIX/bin/i686-elf-strip"

if [ -x "$PREFIX/bin/i686-elf-gcc" ]; then
    make -C ../libmyos all
    make -C libjson all
    make -C libintent all
    make -C llm all
    make -C libgfx all
    make -C libwin all
    make -C libui all
fi

for src in *.c; do
    name="${src%.c}"
    if [ ! -f "$name.elf" ] || [ "$src" -nt "$name.elf" ]; then
        if [ -x "$PREFIX/bin/i686-elf-gcc" ]; then
            "$MYOS_GCC" -O2 "$src" -o "$name.elf"
            echo "Built userland/$name.elf"
        elif [ ! -f "$name.elf" ]; then
            echo "Error: $PREFIX/bin/i686-elf-gcc not found and $name.elf does not exist" >&2
            exit 1
        fi
    fi
done

if [ -f "kilo.elf" ]; then
    cp kilo.elf edit.elf
fi

# Build Lua if present
if [ -d "lua/src" ] && [ -x "$PREFIX/bin/i686-elf-gcc" ]; then
    make -C lua/src posix CC="../../../tools/myos-gcc" AR="$PREFIX/bin/i686-elf-ar rcu" RANLIB="$PREFIX/bin/i686-elf-ranlib"
    cp lua/src/lua lua.elf
    cp lua/src/luac luac.elf
fi

# Strip all ELFs to minimize disk footprint
for elf in *.elf; do
    if [ -f "$elf" ] && [ -x "$STRIP" ]; then
        "$STRIP" -s "$elf" || true
    fi
done
