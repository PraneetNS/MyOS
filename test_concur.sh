#!/bin/bash
set -e
cd /mnt/d/MyOS

LOGFILE=$(mktemp)
(
    sleep 2
    printf "concur_t.elf\n"
    sleep 20
    printf "exit\n"
) | timeout 45s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio -display none -no-reboot > "$LOGFILE" 2>&1 || true

cat "$LOGFILE"
rm -f "$LOGFILE"
