#!/bin/bash
set -e

REPO_DIR="/mnt/d/New folder"

echo "=== Building BusyBox with musl-gcc ==="
cd /root/bb/busybox-1.36.1
make defconfig
sed -i 's/.*CONFIG_STATIC.*/CONFIG_STATIC=y/' .config
sed -i 's/.*CONFIG_TC.*/CONFIG_TC=n/' .config
yes "" | make oldconfig || true
make CC=musl-gcc -j$(nproc)
cp busybox "$REPO_DIR/userland/busybox.elf"
echo "=== BusyBox built successfully ==="

echo "=== Building Lua 5.4.6 with musl-gcc ==="
cd /root/lua-5.4.6/src
musl-gcc -static -o lua lua.o liblua.a -lm
strip lua
cp lua "$REPO_DIR/userland/lua.elf"
echo "=== Lua built successfully ==="

echo "=== Building MicroPython (Python) with musl-gcc ==="
cd /root/micropython/ports/unix
make VARIANT=minimal CC=musl-gcc LDFLAGS_EXTRA="-static" -j$(nproc)
strip build-minimal/micropython
cp build-minimal/micropython "$REPO_DIR/userland/python.elf"
echo "=== MicroPython built successfully ==="

echo "=== Building TCC with musl-gcc ==="
cd /root/tinycc
make clean || true
./configure --prefix=/usr --config-musl --sysincludepaths=/usr/include:/usr/lib/tcc/include --libpaths=/usr/lib --crtprefix=/usr/lib --cc=musl-gcc --extra-cflags="-static -O2" --extra-ldflags="-static"
sed -i 's|XFLAGS-unx = -B$(TOPSRC)|XFLAGS-unx = -B$(TOPSRC) -I$(TOPSRC)/include|' lib/Makefile
make -j$(nproc)
musl-gcc -static -o tcc tcc.o libtcc.a -lm -ldl -lpthread
strip tcc
cp tcc "$REPO_DIR/userland/tcc.elf"
echo "=== TCC built successfully ==="

echo "ALL MUSL TOOLS BUILT SUCCESSFULLY!"
ls -lh "$REPO_DIR/userland/busybox.elf" "$REPO_DIR/userland/lua.elf" "$REPO_DIR/userland/python.elf" "$REPO_DIR/userland/tcc.elf"
