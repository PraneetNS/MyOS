# Stage 18A: Graphics Foundation Architecture & Reference

## Overview
Stage 18A introduces the complete graphics and input subsystem for MyOS:
1. **Multiboot2 Framebuffer & PAT Mapping**: 1024x768 32bpp direct-RGB linear framebuffer boot with Page Attribute Table (PAT) write-combining caching, with automatic fallback to VGA text mode.
2. **Graphical Text Console**: Shadow-buffered 128x48 cell console with embedded PSF2 Terminus 8x16 font, ANSI color & cursor escape sequences, and `KDSETMODE` ioctl support.
3. **Input Subsystem**: PS/2 keyboard and IntelliMouse driver exposing Linux-compatible `/dev/input/mice`, `/dev/input/mouse0`, and `/dev/input/kbd0` nodes delivering `struct input_event` streams.
4. **Framebuffer Device & Memory Mapping**: `/dev/fb0` character device supporting `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, and page-level `mmap()` with PAT write-combining.
5. **Non-blocking I/O & Event Multiplexing**: `poll()` and `select()` syscalls across pipes, TTY, and input devices.
6. **2D Graphics Library (`libgfx`)**: Primitives, Bresenham drawing, anti-aliased TrueType font rendering (`stb_truetype`), multi-format image loading (`stb_image`), and BMP file export.
7. **Demo & Userland Applications**: `gfxdemo`, `paint`, `view`, `screenshot`, `crash_safety_test`, `mousetest`, and `kbdtest`.
8. **Crash Safety**: Kernel-enforced console and keyboard restore on abnormal process termination.

---

## Device Nodes & Ioctls

### `/dev/fb0` (Framebuffer Device)
- **Major/Minor**: DevFS dynamic major/minor.
- **Ioctls**:
  - `FBIOGET_VSCREENINFO`: Fills `struct fb_var_screeninfo` (xres=1024, yres=768, bpp=32, RGB bitfields).
  - `FBIOGET_FSCREENINFO`: Fills `struct fb_fix_screeninfo` (smem_start physical address, smem_len size, line_length=4096).
- **Mmap**:
  - `mmap(NULL, smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`: Maps the linear video memory into the process address space with PAT write-combining caching attributes.

### `/dev/console` (Virtual Terminal & Console Mode Control)
- **Ioctls**:
  - `KDSETMODE`:
    - `KD_TEXT` (0): Restores text console rendering, refreshes screen from shadow buffer.
    - `KD_GRAPHICS` (1): Suspends console output, yields framebuffer access to userland graphics.
  - `KDSKBMODE`:
    - `K_XLATE` (1): TTY interprets keys into ASCII characters and sends them to terminal stdin.
    - `K_OFF` (0): TTY suppresses key input so raw events can be consumed from `/dev/input/kbd0`.
  - `KDGKBMODE`: Queries current keyboard mode.

### `/dev/input/` Subsystem
- `/dev/input/mice`: 3-byte ImPS/2 relative packet stream compatible with standard mouse tools.
- `/dev/input/mouse0`: Linux-compatible `struct input_event` stream (`EV_REL` `REL_X`, `REL_Y`, `REL_WHEEL`, and `EV_KEY` `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`).
- `/dev/input/kbd0`: Linux-compatible `struct input_event` stream (`EV_KEY` keycode events for press/release).

---

## Crash Safety & Safety Nets
- **Kernel-Side Automatic Restore**: When any process exits (normal termination, unhandled signal, or segfault), `console_on_process_exit()` checks if the process held graphics mode or `K_OFF` mode. If so, it automatically restores `KD_TEXT` and `K_XLATE`, redraws the text screen, and frees any device resources.
- **Emergency Hotkey**: Pressing `Ctrl + Alt + F1` in the PS/2 keyboard driver immediately resets `K_XLATE` to guarantee access to the shell.
- **Signal Handlers in Userland Apps**: All graphical applications (`gfxdemo`, `paint`, `view`) trap `SIGINT`, `SIGTERM`, and `SIGQUIT` to restore console state before exit.

---

## Testing & Verification Suite (`tools/qmon.py` and `tools/test_step8.py`)
- **`tools/qmon.py`**: Connects to the QEMU UNIX domain monitor socket to inject keyboard keys (`sendkey`), mouse movements (`mouse_move`), and capture raw screendumps (`screendump`).
- **Deterministic Color Channel Check**: `gfxdemo --still` renders pure reference patches:
  - Red: `(60, 60)` = exact `RGB(255, 0, 0)`
  - Green: `(210, 60)` = exact `RGB(0, 255, 0)`
  - Blue: `(360, 60)` = exact `RGB(0, 0, 255)`
  - Background: `(500, 500)` = exact `RGB(21, 27, 39)`
- **Console Restore Check**: `sendkey esc` triggers clean app shutdown and verified restoration of the graphical text console shell prompt.
- **VGA Fallback Boot**: Booting QEMU with `-vga none` automatically falls back to standard 80x25 VGA text mode without crashing.
