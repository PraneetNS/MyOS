#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PREFIX="${MYOS_PREFIX:-$HOME/opt/myos-cross}"
TARGET="i686-elf"

echo "=== Stage 17 Toolchain Smoke Tests ==="

# 1. Verify compiler and linker versions
echo "[1/4] Checking toolchain binary versions..."
GCC_BIN="$PREFIX/bin/${TARGET}-gcc"
LD_BIN="$PREFIX/bin/${TARGET}-ld"
OBJDUMP_BIN="$PREFIX/bin/${TARGET}-objdump"

if [ ! -x "$GCC_BIN" ]; then
    echo "ERROR: $GCC_BIN not found or not executable"
    exit 1
fi
if [ ! -x "$LD_BIN" ]; then
    echo "ERROR: $LD_BIN not found or not executable"
    exit 1
fi

GCC_VER_OUT="$("$GCC_BIN" --version | head -n 1)"
LD_VER_OUT="$("$LD_BIN" --version | head -n 1)"

echo "  $GCC_VER_OUT"
echo "  $LD_VER_OUT"

if ! echo "$GCC_VER_OUT" | grep -q "13.2.0"; then
    echo "ERROR: Expected GCC 13.2.0, got: $GCC_VER_OUT"
    exit 1
fi
if ! echo "$LD_VER_OUT" | grep -q "2.42"; then
    echo "ERROR: Expected Binutils ld 2.42, got: $LD_VER_OUT"
    exit 1
fi
echo "  -> Version checks PASSED."

# 2. Verify Sysroot Libraries and Headers
echo "[2/4] Checking Sysroot libraries and headers..."
SYSROOT="$PREFIX/$TARGET"

for lib in libc.a libm.a libg.a; do
    if [ ! -f "$SYSROOT/lib/$lib" ]; then
        echo "ERROR: $SYSROOT/lib/$lib does not exist"
        exit 1
    fi
    echo "  Found $SYSROOT/lib/$lib ($(du -h "$SYSROOT/lib/$lib" | awk '{print $1}'))"
done

if [ ! -f "$SYSROOT/include/stdio.h" ]; then
    echo "ERROR: $SYSROOT/include/stdio.h does not exist"
    exit 1
fi
echo "  Found $SYSROOT/include/stdio.h"
echo "  -> Sysroot checks PASSED."

# 3. Compile and Link Test Program using tools/myos-gcc
echo "[3/4] Compiling and linking test program with tools/myos-gcc..."
TMP_DIR="$(mktemp -d /tmp/myos-smoke-XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Prepare throwaway libmyos if not already fully implemented
THROWAWAY_CREATED=0
if [ ! -f "$ROOT_DIR/libmyos/libmyos.a" ] || [ ! -f "$ROOT_DIR/libmyos/crt0.o" ]; then
    mkdir -p "$ROOT_DIR/libmyos"
    THROWAWAY_CREATED=1

    cat << 'EOF' > "$TMP_DIR/crt0.s"
.global _start
.extern main
.extern exit
_start:
    pushl $0
    pushl $0
    pushl $0
    call main
    pushl %eax
    call exit
1:  hlt
    jmp 1b
EOF
    "$GCC_BIN" -c "$TMP_DIR/crt0.s" -o "$ROOT_DIR/libmyos/crt0.o"

    cat << 'EOF' > "$TMP_DIR/stubs.c"
#include <sys/stat.h>
void _exit(int status) { while (1); }
int _close(int file) { return -1; }
int _fstat(int file, struct stat *st) { return 0; }
int _isatty(int file) { return 1; }
int _lseek(int file, int ptr, int dir) { return 0; }
int _read(int file, char *ptr, int len) { return 0; }
int _write(int file, const char *ptr, int len) { return len; }
void *_sbrk(int incr) {
    extern char _end;
    static char *heap = 0;
    if (!heap) heap = &_end;
    char *prev = heap;
    heap += incr;
    return prev;
}
int _kill(int pid, int sig) { return -1; }
int _getpid(void) { return 1; }

/* POSIX aliases */
int close(int f) { return _close(f); }
int fstat(int f, struct stat *st) { return _fstat(f, st); }
int isatty(int f) { return _isatty(f); }
int lseek(int f, int p, int d) { return _lseek(f, p, d); }
int read(int f, char *b, int l) { return _read(f, b, l); }
int write(int f, const char *b, int l) { return _write(f, b, l); }
void *sbrk(int i) { return _sbrk(i); }
int kill(int p, int s) { return _kill(p, s); }
int getpid(void) { return _getpid(); }
EOF
    "$GCC_BIN" -c -O2 "$TMP_DIR/stubs.c" -o "$TMP_DIR/stubs.o"
    "$PREFIX/bin/${TARGET}-ar" rcs "$ROOT_DIR/libmyos/libmyos.a" "$TMP_DIR/stubs.o"
fi

cat << 'EOF' > "$TMP_DIR/smoke.c"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <setjmp.h>

jmp_buf jmpbuf;

int main(int argc, char **argv) {
    if (setjmp(jmpbuf) == 0) {
        void *p = malloc(256);
        double val = sqrt(4.0);
        printf("Smoke test: sqrt=%f, ptr=%p\n", val, p);
        free(p);
        longjmp(jmpbuf, 1);
    }
    return 0;
}
EOF

"$ROOT_DIR/tools/myos-gcc" "$TMP_DIR/smoke.c" -o "$TMP_DIR/smoke.elf"
if [ ! -f "$TMP_DIR/smoke.elf" ]; then
    echo "ERROR: Failed to produce smoke.elf"
    exit 1
fi
echo "  Successfully compiled and linked smoke.elf"

# Clean up throwaway stubs if we created them specifically for this smoke test
if [ "$THROWAWAY_CREATED" -eq 1 ]; then
    rm -f "$ROOT_DIR/libmyos/libmyos.a" "$ROOT_DIR/libmyos/crt0.o"
    # remove libmyos if empty
    rmdir "$ROOT_DIR/libmyos" 2>/dev/null || true
fi
echo "  -> Compilation and link checks PASSED."

# 4. Check ELF Format
echo "[4/4] Verifying ELF format with objdump..."
OBJDUMP_OUT="$("$OBJDUMP_BIN" -f "$TMP_DIR/smoke.elf")"
echo "$OBJDUMP_OUT"

if ! echo "$OBJDUMP_OUT" | grep -q "file format elf32-i386"; then
    echo "ERROR: Expected file format elf32-i386"
    exit 1
fi
if ! echo "$OBJDUMP_OUT" | grep -q "architecture: i386"; then
    echo "ERROR: Expected architecture i386"
    exit 1
fi
echo "  -> ELF architecture checks PASSED."

echo "=== ALL TOOLCHAIN SMOKE TESTS PASSED! ==="
