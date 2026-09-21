#!/usr/bin/env python3
"""Builds disk.img for MyOS: a superblock, a flat directory table, and
contiguous file data -- matching the on-disk layout src/fs.c expects
exactly. See src/fs.c's comments for the format if you change this.
"""
import struct
import sys
import os

SECTOR = 512
MAGIC = 0x4D594653          # must match FS_MAGIC in src/fs.c
DIR_START_LBA = 1
ENTRY_SIZE = 64             # must match fs_entry_disk_t in src/fs.c
ENTRIES_PER_SECTOR = SECTOR // ENTRY_SIZE
NAME_LEN = 48                # must match FS_MAX_NAME in src/fs.h
DATA_START_LBA = 16          # first LBA available for file data

def build(files, out_path):
    """files: list of (name_on_disk, host_path)"""
    file_count = len(files)
    dir_sectors = (file_count + ENTRIES_PER_SECTOR - 1) // ENTRIES_PER_SECTOR

    # --- read file contents, assign each a start LBA ---
    blobs = []
    next_lba = DATA_START_LBA
    entries = []
    for name, path in files:
        with open(path, "rb") as f:
            data = f.read()
        size = len(data)
        sectors = (size + SECTOR - 1) // SECTOR
        entries.append((name, next_lba, size))
        blobs.append(data.ljust(sectors * SECTOR, b"\x00"))
        next_lba += sectors

    total_sectors = next_lba

    image = bytearray(total_sectors * SECTOR)

    # --- superblock (LBA 0) ---
    sb = struct.pack("<II", MAGIC, file_count)
    sb = sb.ljust(SECTOR, b"\x00")
    image[0:SECTOR] = sb

    # --- directory table (starting LBA 1) ---
    dirbuf = bytearray(dir_sectors * SECTOR)
    for i, (name, lba, size) in enumerate(entries):
        name_bytes = name.encode("ascii")[:NAME_LEN - 1].ljust(NAME_LEN, b"\x00")
        entry = name_bytes + struct.pack("<II", lba, size) + b"\x00" * 8
        assert len(entry) == ENTRY_SIZE
        dirbuf[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = entry
    image[DIR_START_LBA * SECTOR: DIR_START_LBA * SECTOR + len(dirbuf)] = dirbuf

    # --- file data ---
    for (name, lba, size), blob in zip(entries, blobs):
        off = lba * SECTOR
        image[off:off + len(blob)] = blob

    with open(out_path, "wb") as f:
        f.write(image)

    print(f"Built {out_path}: {file_count} file(s), {total_sectors} sectors "
          f"({total_sectors * SECTOR} bytes)")
    for name, lba, size in entries:
        print(f"  {name:20s} LBA {lba:4d}  {size} bytes")

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    files = [
        ("hello.txt", os.path.join(here, "hello.txt")),
        ("hello.elf", os.path.join(root, "userland", "hello.elf")),
        ("badwrite.elf", os.path.join(root, "userland", "badwrite.elf")),
    ]

    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "disk.img")
    build(files, out)
