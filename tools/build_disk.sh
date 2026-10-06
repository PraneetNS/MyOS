#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."

IMAGE="disk.img"
echo "Creating 32MB disk image..."
rm -f "$IMAGE"
dd if=/dev/zero of="$IMAGE" bs=1M count=32 status=none

echo "Formatting disk image with FAT16..."
mkfs.fat -F 16 -n "MYOS" "$IMAGE"

echo "Creating /bin directory..."
mmd -i "$IMAGE" "::/bin"

echo "Building userland..."
bash userland/build.sh

echo "Copying userland executables..."
for elf in userland/*.elf; do
    if [ -f "$elf" ]; then
        name=$(basename "$elf")
        base="${name%.elf}"
        mcopy -i "$IMAGE" "$elf" "::/$name"
        mcopy -i "$IMAGE" "$elf" "::/bin/$name"
        if [ "$base" != "$name" ]; then
            mcopy -i "$IMAGE" "$elf" "::/$base"
            mcopy -i "$IMAGE" "$elf" "::/bin/$base"
        fi
    fi
done

echo "Copying data files..."
if [ -f "tools/hello.txt" ]; then
    mcopy -i "$IMAGE" tools/hello.txt "::/hello.txt"
fi

echo "Hello from FAT16 on MyOS!" > /tmp/fat_hello.txt
mcopy -i "$IMAGE" /tmp/fat_hello.txt "::/fat.txt"
rm -f /tmp/fat_hello.txt

echo "Disk image built successfully:"
mdir -i "$IMAGE" ::
