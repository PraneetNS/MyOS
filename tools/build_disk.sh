#!/bin/bash
set -e
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."

IMAGE="disk.img"
echo "Creating 64MB disk image..."
rm -f "$IMAGE"
dd if=/dev/zero of="$IMAGE" bs=1M count=64 status=none

echo "Formatting disk image with FAT16..."
mkfs.fat -F 16 -n "MYOS" "$IMAGE"

echo "Creating /bin, /tmp, /dev, /etc, /proc, and /home directories..."
mmd -i "$IMAGE" "::/bin"
mmd -i "$IMAGE" "::/tmp"
mmd -i "$IMAGE" "::/dev"
mmd -i "$IMAGE" "::/etc"
mmd -i "$IMAGE" "::/proc"
mmd -i "$IMAGE" "::/home"
mmd -i "$IMAGE" "::/share" || true
mmd -i "$IMAGE" "::/share/fonts" || true

echo "Setting /etc/timezone to 330..."
printf "330\n" > /tmp/myos_timezone
mcopy -i "$IMAGE" /tmp/myos_timezone "::/etc/timezone"
rm -f /tmp/myos_timezone

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

if [ -f "userland/demo.lua" ]; then
    mcopy -i "$IMAGE" userland/demo.lua "::/home/demo.lua"
fi

if [ -f "userland/alloc.lua" ]; then
    mcopy -i "$IMAGE" userland/alloc.lua "::/home/alloc.lua"
fi

if [ -f "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" ]; then
    echo "Copying DejaVuSans font..."
    mcopy -i "$IMAGE" /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "::/share/fonts/dejavu.ttf"
fi

python3 -c "
import struct
w, h = 128, 128
row_padded = (w * 3 + 3) & ~3
img_size = row_padded * h
file_size = 54 + img_size
header = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 54)
dib = struct.pack('<IIIHHIIIIII', 40, w, h, 1, 24, 0, img_size, 2835, 2835, 0, 0)
pixels = bytearray(img_size)
for y in range(h):
    row_offset = (h - 1 - y) * row_padded
    for x in range(w):
        b = int(255 * x / w)
        g = int(255 * y / h)
        r = int(255 * (w - x) / w)
        pixels[row_offset + x*3 : row_offset + x*3 + 3] = bytes([b, g, r])
with open('/tmp/sample.bmp', 'wb') as f:
    f.write(header + dib + pixels)
"
mcopy -i "$IMAGE" /tmp/sample.bmp "::/share/sample.bmp"
rm -f /tmp/sample.bmp

echo "Hello from FAT16 on MyOS!" > /tmp/fat_hello.txt
mcopy -i "$IMAGE" /tmp/fat_hello.txt "::/fat.txt"
rm -f /tmp/fat_hello.txt

python3 -c "print('LARGE_FILE_START_' + 'X'*4000 + '_LARGE_FILE_END')" > /tmp/large.txt
mcopy -i "$IMAGE" /tmp/large.txt "::/large.txt"
rm -f /tmp/large.txt

python3 -c "import sys; sys.stdout.write('A'*70000)" > /tmp/big.txt
mcopy -i "$IMAGE" /tmp/big.txt "::/big.txt"
mcopy -i "$IMAGE" /tmp/big.txt "::/big"
rm -f /tmp/big.txt

echo "Disk image built successfully:"
mdir -i "$IMAGE" ::
