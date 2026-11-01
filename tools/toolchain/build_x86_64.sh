#!/bin/bash
set -e

PREFIX="${MYOS_PREFIX:-/root/opt/myos-cross}"
TARGET="x86_64-elf"
JOBS="$(nproc 2>/dev/null || echo 16)"

SRC_DIR="/root/toolchain_src"
BUILD_DIR="/root/toolchain_build"
LOG_FILE="/root/toolchain_build_x86_64.log"

BINUTILS_VER="2.42"
GCC_VER="13.2.0"
NEWLIB_VER="4.4.0.20231231"

export PATH="$PREFIX/bin:$PATH"

mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX"
echo "=== x86_64-elf Toolchain Build Started: $(date) ===" | tee "$LOG_FILE"
echo "Prefix: $PREFIX" | tee -a "$LOG_FILE"
echo "Target: $TARGET" | tee -a "$LOG_FILE"
echo "Jobs:   $JOBS"   | tee -a "$LOG_FILE"

cd "$SRC_DIR"
if [ ! -d "binutils-${BINUTILS_VER}" ]; then
    echo "Extracting binutils-${BINUTILS_VER}..." | tee -a "$LOG_FILE"
    tar -xf "binutils-${BINUTILS_VER}.tar.xz"
fi

if [ ! -d "gcc-${GCC_VER}" ]; then
    echo "Extracting gcc-${GCC_VER}..." | tee -a "$LOG_FILE"
    tar -xf "gcc-${GCC_VER}.tar.xz"
fi

if [ ! -d "newlib-${NEWLIB_VER}" ]; then
    echo "Extracting newlib-${NEWLIB_VER}..." | tee -a "$LOG_FILE"
    tar -xf "newlib-${NEWLIB_VER}.tar.gz"
fi

# 1. Build Binutils for x86_64-elf
if [ -x "$PREFIX/bin/${TARGET}-as" ] && [ -x "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Binutils for $TARGET already installed." | tee -a "$LOG_FILE"
else
    echo "=== Building Binutils for $TARGET ===" | tee -a "$LOG_FILE"
    rm -rf "$BUILD_DIR/binutils-x86_64"
    mkdir -p "$BUILD_DIR/binutils-x86_64"
    cd "$BUILD_DIR/binutils-x86_64"
    "$SRC_DIR/binutils-${BINUTILS_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot \
        --disable-nls \
        --disable-werror >> "$LOG_FILE" 2>&1
    make -j"$JOBS" >> "$LOG_FILE" 2>&1
    make install >> "$LOG_FILE" 2>&1
    echo "Binutils for $TARGET installed successfully." | tee -a "$LOG_FILE"
fi

# 2. Build GCC (bootstrap) for x86_64-elf
if [ -x "$PREFIX/bin/${TARGET}-gcc" ]; then
    echo "GCC for $TARGET already installed." | tee -a "$LOG_FILE"
else
    echo "=== Building GCC for $TARGET ===" | tee -a "$LOG_FILE"
    rm -rf "$BUILD_DIR/gcc-x86_64"
    mkdir -p "$BUILD_DIR/gcc-x86_64"
    cd "$BUILD_DIR/gcc-x86_64"
    "$SRC_DIR/gcc-${GCC_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-nls \
        --enable-languages=c \
        --without-headers \
        --disable-multilib >> "$LOG_FILE" 2>&1
    make -j"$JOBS" all-gcc >> "$LOG_FILE" 2>&1
    make -j"$JOBS" all-target-libgcc >> "$LOG_FILE" 2>&1
    make install-gcc >> "$LOG_FILE" 2>&1
    make install-target-libgcc >> "$LOG_FILE" 2>&1
    echo "GCC for $TARGET installed successfully." | tee -a "$LOG_FILE"
fi

# 3. Build Newlib for x86_64-elf
if [ -f "$PREFIX/${TARGET}/lib/libc.a" ]; then
    echo "Newlib for $TARGET already installed." | tee -a "$LOG_FILE"
else
    echo "=== Building Newlib for $TARGET ===" | tee -a "$LOG_FILE"
    rm -rf "$BUILD_DIR/newlib-x86_64"
    mkdir -p "$BUILD_DIR/newlib-x86_64"
    cd "$BUILD_DIR/newlib-x86_64"
    "$SRC_DIR/newlib-${NEWLIB_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-newlib-supplied-syscalls \
        --enable-newlib-io-long-long \
        --enable-newlib-io-float \
        --disable-multilib >> "$LOG_FILE" 2>&1
    make -j"$JOBS" >> "$LOG_FILE" 2>&1
    make install >> "$LOG_FILE" 2>&1
    echo "Newlib for $TARGET installed successfully." | tee -a "$LOG_FILE"
fi

echo "=== All done building $TARGET toolchain! ===" | tee -a "$LOG_FILE"

