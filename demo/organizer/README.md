# Demo 1: File Organizer Agent

## Overview
The **Organizer** demo showcases autonomous agent desktop interaction and human-in-the-loop approval gating:
1. Agent launches with principal `organizer`.
2. Inspects desktop state using the Accessibility Tree (`ui.tree`) on the Files application (`/bin/files.elf`).
3. Enumerates files and generates a semantic summary of the directory contents.
4. Attempts a destructive action: calling `fs.delete` to remove an untracked temporary file.
5. The kernel and window manager intercept the action, raising the secure Human Approval Dialog.
6. The user explicitly clicks **Deny** (or approves).
7. The operation is aborted, and an immutable cryptographic audit record is committed to `/proc/agents/log`.

## How to Run

### Via Make & Headless Automated Recorder:
```bash
make demo-organizer
# or
./tools/record_demo.sh organizer
```

### Manually inside MyOS:
1. Boot the OS: `qemu-system-i386 -m 128 -drive file=disk.img,format=raw`
2. Open **Agent Panel** from desktop or run:
   ```bash
   agentctl create --name organizer --principal organizer --caps "CAP_WINDOW_ACT:files,CAP_BUS_CALL:fs.,CAP_BUS_CALL:model." /bin/files.elf
   ```
3. Observe the approval prompt and check audit log:
   ```bash
   cat /proc/agents/log
   ```

