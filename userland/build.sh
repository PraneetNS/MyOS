#!/bin/bash
# Builds every userland/*.c program using tools/myos-gcc and Newlib libc.
set -e
cd "$(dirname "$0")"

MYOS_GCC="../tools/myos-gcc"
if [ -z "$MYOS_PREFIX" ]; then
    if [ -d "/root/opt/myos-cross" ]; then
        PREFIX="/root/opt/myos-cross"
    else
        PREFIX="$HOME/opt/myos-cross"
    fi
else
    PREFIX="$MYOS_PREFIX"
fi
STRIP="$PREFIX/bin/x86_64-elf-strip"

if [ -x "$PREFIX/bin/x86_64-elf-gcc" ]; then
    make -C ../libmyos all
    make -C libjson all
    make -C libintent all
    make -C libtls all
    make -C llm all
    make -C libgfx all
    make -C libwin all
    make -C libui all
fi

for src in *.c; do
    name="${src%.c}"
    if [ ! -f "$name.elf" ] || [ "$src" -nt "$name.elf" ] || [ "../libmyos/libmyos.a" -nt "$name.elf" ] || [ "../libmyos/crt0.o" -nt "$name.elf" ]; then
        if [ -x "$PREFIX/bin/x86_64-elf-gcc" ]; then
            "$MYOS_GCC" -O2 "$src" -o "$name.elf"
            echo "Built userland/$name.elf"
        elif [ ! -f "$name.elf" ]; then
            echo "Error: $PREFIX/bin/x86_64-elf-gcc not found and $name.elf does not exist" >&2
            exit 1
        fi
    fi
done

if [ -f "kilo.elf" ]; then
    cp kilo.elf edit.elf
fi

# Build Lua if present
if [ -d "lua/src" ] && [ -x "$PREFIX/bin/x86_64-elf-gcc" ]; then
    make -C lua/src posix CC="../../../tools/myos-gcc" AR="$PREFIX/bin/x86_64-elf-ar rcu" RANLIB="$PREFIX/bin/x86_64-elf-ranlib"
    cp lua/src/lua lua.elf
    cp lua/src/luac luac.elf
fi

# Strip all ELFs to minimize disk footprint
for elf in *.elf; do
    if [ -f "$elf" ] && [ -x "$STRIP" ]; then
        "$STRIP" -s "$elf" || true
    fi
done
