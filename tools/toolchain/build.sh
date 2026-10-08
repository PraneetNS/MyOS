#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOG_FILE="$SCRIPT_DIR/build.log"

PREFIX="${MYOS_PREFIX:-$HOME/opt/myos-cross}"
TARGET="i686-elf"
JOBS="$(nproc 2>/dev/null || echo 4)"

SRC_DIR="$SCRIPT_DIR/src"
BUILD_DIR="$SCRIPT_DIR/build"

BINUTILS_VER="2.42"
BINUTILS_URL="https://ftpmirror.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
BINUTILS_SHA256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"

GCC_VER="13.2.0"
GCC_URL="https://ftpmirror.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
GCC_SHA256="e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da"

NEWLIB_VER="4.4.0.20231231"
NEWLIB_URL="https://sourceware.org/pub/newlib/newlib-${NEWLIB_VER}.tar.gz"
NEWLIB_SHA256="0c166a39e1bf0951dfafcd68949fe0e4b6d3658081d6282f39aeefc6310f2f13"

export PATH="$PREFIX/bin:$PATH"

mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX"
echo "=== Toolchain Build Started: $(date) ===" > "$LOG_FILE"
echo "Prefix: $PREFIX" | tee -a "$LOG_FILE"
echo "Target: $TARGET" | tee -a "$LOG_FILE"
echo "Jobs:   $JOBS"   | tee -a "$LOG_FILE"

download_and_verify() {
    local url="$1"
    local file="$2"
    local expected_hash="$3"

    cd "$SRC_DIR"
    if [ ! -f "$file" ]; then
        echo "Downloading $file..." | tee -a "$LOG_FILE"
        wget -q --show-progress "$url" -O "$file" 2>&1 | tee -a "$LOG_FILE"
    fi

    echo "Verifying checksum for $file..." | tee -a "$LOG_FILE"
    local actual_hash
    actual_hash="$(sha256sum "$file" | awk '{print $1}')"
    if [ "$actual_hash" != "$expected_hash" ]; then
        echo "ERROR: Checksum mismatch for $file!" | tee -a "$LOG_FILE"
        echo "Expected: $expected_hash" | tee -a "$LOG_FILE"
        echo "Actual:   $actual_hash" | tee -a "$LOG_FILE"
        exit 1
    fi
    echo "Checksum verified: $file" | tee -a "$LOG_FILE"
}

# 1. Download & Extract only what is needed

# 1. Download & Extract only what is needed

# Binutils
if [ -f "$PREFIX/.built-binutils" ] && [ -x "$PREFIX/bin/${TARGET}-as" ] && [ -x "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Binutils already installed at $PREFIX, skipping download/extract." | tee -a "$LOG_FILE"
else
    if [ ! -d "$SRC_DIR/binutils-${BINUTILS_VER}" ]; then
        download_and_verify "$BINUTILS_URL" "binutils-${BINUTILS_VER}.tar.xz" "$BINUTILS_SHA256"
        echo "Extracting binutils-${BINUTILS_VER}..." | tee -a "$LOG_FILE"
        tar -xf "$SRC_DIR/binutils-${BINUTILS_VER}.tar.xz" -C "$SRC_DIR"
        rm -f "$SRC_DIR/binutils-${BINUTILS_VER}.tar.xz"
    fi
fi

# GCC
if [ -f "$PREFIX/.built-gcc" ] && [ -x "$PREFIX/bin/${TARGET}-gcc" ] && [ -f "$PREFIX/lib/gcc/${TARGET}/${GCC_VER}/libgcc.a" ]; then
    echo "GCC already installed at $PREFIX, skipping download/extract." | tee -a "$LOG_FILE"
else
    if [ ! -d "$SRC_DIR/gcc-${GCC_VER}" ]; then
        download_and_verify "$GCC_URL" "gcc-${GCC_VER}.tar.xz" "$GCC_SHA256"
        echo "Extracting gcc-${GCC_VER}..." | tee -a "$LOG_FILE"
        tar -xf "$SRC_DIR/gcc-${GCC_VER}.tar.xz" -C "$SRC_DIR"
        rm -f "$SRC_DIR/gcc-${GCC_VER}.tar.xz"
    fi
fi

# Newlib
if [ -f "$PREFIX/.built-newlib" ] && [ -f "$PREFIX/${TARGET}/lib/libc.a" ] && [ -f "$PREFIX/${TARGET}/lib/libm.a" ]; then
    echo "Newlib already installed at $PREFIX, skipping download/extract." | tee -a "$LOG_FILE"
else
    if [ ! -d "$SRC_DIR/newlib-${NEWLIB_VER}" ]; then
        download_and_verify "$NEWLIB_URL" "newlib-${NEWLIB_VER}.tar.gz" "$NEWLIB_SHA256"
        echo "Extracting newlib-${NEWLIB_VER}..." | tee -a "$LOG_FILE"
        tar -xf "$SRC_DIR/newlib-${NEWLIB_VER}.tar.gz" -C "$SRC_DIR"
        rm -f "$SRC_DIR/newlib-${NEWLIB_VER}.tar.gz"
    fi
fi

# 2. Build Binutils
if [ -f "$PREFIX/.built-binutils" ] && [ -x "$PREFIX/bin/${TARGET}-as" ] && [ -x "$PREFIX/bin/${TARGET}-ld" ]; then
    echo "Binutils already installed at $PREFIX, skipping build." | tee -a "$LOG_FILE"
else
    echo "=== Building Binutils ${BINUTILS_VER} ===" | tee -a "$LOG_FILE"
    mkdir -p "$BUILD_DIR/binutils"
    cd "$BUILD_DIR/binutils"
    "$SRC_DIR/binutils-${BINUTILS_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot \
        --disable-nls \
        --disable-werror >> "$LOG_FILE" 2>&1
    make -j"$JOBS" >> "$LOG_FILE" 2>&1
    make install >> "$LOG_FILE" 2>&1
    echo "Binutils installed successfully." | tee -a "$LOG_FILE"
    touch "$PREFIX/.built-binutils"
    echo "Removing Binutils build and source trees to save space..." | tee -a "$LOG_FILE"
    rm -rf "$BUILD_DIR/binutils" "$SRC_DIR/binutils-${BINUTILS_VER}"
fi

# 3. Build GCC (bootstrap)
if [ -f "$PREFIX/.built-gcc" ] && [ -x "$PREFIX/bin/${TARGET}-gcc" ] && [ -f "$PREFIX/lib/gcc/${TARGET}/${GCC_VER}/libgcc.a" ]; then
    echo "GCC already installed at $PREFIX, skipping build." | tee -a "$LOG_FILE"
else
    echo "=== Building GCC ${GCC_VER} ===" | tee -a "$LOG_FILE"
    mkdir -p "$BUILD_DIR/gcc"
    cd "$BUILD_DIR/gcc"
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
    echo "GCC installed successfully." | tee -a "$LOG_FILE"
    touch "$PREFIX/.built-gcc"
    echo "Removing GCC build and source trees to save space..." | tee -a "$LOG_FILE"
    rm -rf "$BUILD_DIR/gcc" "$SRC_DIR/gcc-${GCC_VER}"
fi

# 4. Build Newlib
if [ -f "$PREFIX/.built-newlib" ] && [ -f "$PREFIX/${TARGET}/lib/libc.a" ] && [ -f "$PREFIX/${TARGET}/lib/libm.a" ]; then
    echo "Newlib already installed at $PREFIX, skipping build." | tee -a "$LOG_FILE"
else
    echo "=== Building Newlib ${NEWLIB_VER} ===" | tee -a "$LOG_FILE"
    mkdir -p "$BUILD_DIR/newlib"
    cd "$BUILD_DIR/newlib"
    "$SRC_DIR/newlib-${NEWLIB_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-newlib-supplied-syscalls \
        --enable-newlib-io-long-long \
        --enable-newlib-io-float \
        --disable-multilib >> "$LOG_FILE" 2>&1
    make -j"$JOBS" >> "$LOG_FILE" 2>&1
    make install >> "$LOG_FILE" 2>&1
    echo "Newlib installed successfully." | tee -a "$LOG_FILE"
    touch "$PREFIX/.built-newlib"
    echo "Removing Newlib build and source trees to save space..." | tee -a "$LOG_FILE"
    rm -rf "$BUILD_DIR/newlib" "$SRC_DIR/newlib-${NEWLIB_VER}"
fi

echo "Cleaning up temporary source and build trees to conserve disk space..." | tee -a "$LOG_FILE"
rm -rf "$BUILD_DIR"
rm -rf "$SRC_DIR"

echo "=== Toolchain Build Finished Successfully: $(date) ===" | tee -a "$LOG_FILE"
exit 0
