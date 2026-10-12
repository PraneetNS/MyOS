# FAT16 Long Filename (LFN) Architecture & Implementation Plan

## 1. System Audit & Buffer Size Analysis

### Current Limitations (Pre-LFN)
1. **Name Limits & Directory Entries (`struct dirent`)**:
   - `src/vfs.h`: `struct dirent.d_name` was hardcoded to `char d_name[64]`.
   - `libmyos/include/dirent.h`: `struct dirent.d_name` was `char d_name[64]`.
   - Neither header defined standard POSIX `NAME_MAX` or `PATH_MAX`.
   - `src/fat16.c`: Directory entries only handled 8.3 FAT short names (`to_dos_name` and `from_dos_name`). VFAT entries with attribute `0x0F` (`FAT_ATTR_LFN`) were explicitly ignored during directory iteration (`readdir`), and creation/lookup relied on 8.3 truncation.

2. **Kernel Path & Token Buffers**:
   - `src/vfs.c`:
     - `vfs_resolve_path`: `canon[128]`, `token[64]`.
     - `vfs_resolve_parent`: `parent_path[128]`.
     - `vfs_path_canonical`: `combined[128]`, `segs[16][32]`. Any path segment exceeding 31 characters was truncated.
   - `src/syscall.c`:
     - `SYS_EXEC`: `kname[64]`, `bin_path[64]`.
     - `SYS_OPEN`, `SYS_MKDIR`, `SYS_RMDIR`, `SYS_UNLINK`: `child_name[64]`.
     - `SYS_RENAME`: `old_child[64]`, `new_child[64]`.
   - `src/process.h`:
     - `process_t.cwd[64]`: Current working directory buffer was only 64 bytes.

3. **Userland Tools**:
   - `userland/sh.c`: `line[256]`, `token_buf[512]`, `cwd[64]`.
   - `userland/ls.c`: `item_path[128]`.
   - `tools/build_disk.sh`: Generates short aliases (e.g. `screensh.elf`, `crash_sa.elf`) as workarounds for 8.3 limits.

---

## 2. Universal Limits & Error Handling

- **`NAME_MAX = 255`**: Maximum length of a single path component in UTF-8 bytes (excluding NUL).
- **`PATH_MAX = 1024`**: Maximum length of an absolute or relative path string (including NUL terminator).
- **`struct dirent`**: Expanded `d_name` from 64 to 256 bytes (`NAME_MAX + 1`).
- **`-ENAMETOOLONG`**:
  - Returned when any individual component length exceeds `NAME_MAX`.
  - Returned when total canonical or input path length exceeds `PATH_MAX`.

---

## 3. VFAT LFN Specifications & On-Disk Format

### VFAT Directory Entry Structure (32 bytes)
```c
typedef struct {
    uint8_t  seq;          /* Sequence number: 1..31 | 0x40 (LAST_LFN) */
    uint16_t name1[5];     /* Characters 1-5 (UTF-16LE) */
    uint8_t  attr;         /* Always 0x0F (FAT_ATTR_LFN = READONLY|HIDDEN|SYSTEM|VOLUME_ID) */
    uint8_t  type;         /* Reserved, always 0x00 */
    uint8_t  checksum;     /* Checksum of the associated 8.3 alias */
    uint16_t name2[6];     /* Characters 6-11 (UTF-16LE) */
    uint16_t fst_clus_lo;  /* Reserved, always 0x0000 */
    uint16_t name3[2];     /* Characters 12-13 (UTF-16LE) */
} __attribute__((packed)) fat_lfn_entry_t;
```

### Key VFAT Rules
1. **Reverse Order**: LFN entries precede their corresponding 8.3 short entry on disk in reverse order (entry with sequence `N | 0x40` comes first, followed by `N-1`, ..., `1`, then the short entry).
2. **Checksum**: The 8-bit checksum is calculated over the 11-byte short name:
   ```c
   uint8_t chk = 0;
   for (int i = 0; i < 11; i++) {
       chk = ((chk & 1) ? 0x80 : 0) + (chk >> 1) + (uint8_t)short_name[i];
   }
   ```
3. **Padding**: After the NUL terminator `0x0000`, unused UTF-16 character slots are padded with `0xFFFF`.
4. **NT Case Flags (Byte 12 / `nt_res`)**:
   - `0x08`: Base name is all lowercase.
   - `0x10`: Extension is all lowercase.
   When an 8.3 name fits completely without needing LFN entries, these flags preserve case without consuming extra directory slots.

---

## 4. Multi-Step Implementation Plan

- **Step 1: Audit + Name Limits**:
  - Update `src/vfs.h`, `libmyos/include/dirent.h`, `src/process.h`.
  - Update path buffers in `src/vfs.c`, `src/syscall.c`, `userland/sh.c`, `userland/ls.c`.
  - Add `-ENAMETOOLONG` checks for component and total path lengths.
- **Step 2: LFN Read Support**:
  - Parse VFAT entries in `fat16_readdir` and `fat16_lookup`.
  - Convert UTF-16LE to UTF-8.
  - Implement case-insensitive comparison against both long name and 8.3 alias.
- **Step 3: LFN Write Support**:
  - Name validation and short alias generation (`~1` through `~N`).
  - Consecutive slot allocation in root directory and subdirectories.
  - Write reverse sequence LFN entries with valid checksums and padding.
- **Step 4: Rename, Unlink, Rmdir**:
  - Mark entire LFN sets as `0xE5` deleted entries on deletion.
  - Support case-only rename (`foo.txt` -> `Foo.txt`) and cross-directory moves.
- **Step 5: Clean Up 8.3 Workarounds**:
  - Remove short aliases from `tools/build_disk.sh`.
  - Restore natural long filenames for executables and font assets.
- **Step 6: Automated Test Suite**:
  - `userland/lfntest.c` testing UTF-8, alias collisions, directory cluster growth, model-based stress, and host interop (`fsck.fat -n`, `mdir`, `mcopy`).
