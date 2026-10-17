# Stage 18B-2: Window System & Desktop Architecture

## Overview
Stage 18B-2 introduces a lightweight, robust, asynchronous client-server window system and desktop environment for MyOS.

Key Subsystems:
1. **Display Server / Window Manager (`wm`)**:
   - Takes display ownership via `KD_GRAPHICS` and `K_OFF`.
   - Listens on AF_UNIX stream socket `/run/display.sock`.
   - Composes client window surfaces into a 32bpp RAM back buffer and flushes only dirty rectangles to `/dev/fb0`.
   - Renders server-side window decorations, titles, controls, drop shadows, and a software mouse cursor.
   - Translates raw scancodes into Unicode using `/usr/share/keymaps/us.map`.
   - Enforces fault isolation against buggy or malicious clients (truncation, SIGBUS protection, buffer bounds validation).
2. **Window System Protocol**:
   - Binary framing: `{uint32_t len, uint16_t type, uint16_t flags, uint32_t window_id}` (little-endian, max 4KB).
   - Fixed-size message structs with strict payload validation.
   - Out-of-band shared memory buffers created under `/dev/shm/win-<pid>-<id>-<n>`.
3. **Client Library (`libwin`)**:
   - Simple, high-performance static library (`libwin.a`) providing window creation, double-buffered rendering surface (`gfx_surface_t`), damage reporting, atomic commits, event dispatching, resize handling, and clipboard integration.
4. **Widget Toolkit (`libui`)**:
   - Retained-lite UI toolkit with labels, buttons, checkboxes, radio buttons, text inputs, text areas, scrollable listboxes, scrollbars, tabs, menus, dialogs, and layout containers.
5. **Desktop Applications & Utilities**:
   - `wmctl`: Command-line inspection and control tool over `/run/display.sock`.
   - `winhello`: Minimal test client validating protocol events and rendering.
   - `uidemo`: Showcase application for all `libui` widgets.
   - `term`: VT100/xterm-256color terminal emulator over PTY (`forkpty`).
   - `files`: File manager with LFN/UTF-8 support, path navigation, file execution, and deletion.
   - `edit`: Graphical text editor with syntax/line numbering, search/replace, and undo/redo.
   - `sysmon`: System task manager with process inspection, graphs, and kill actions.
   - `viewer`: Image viewer for BMP and image formats.
   - `panel`: Top panel with application launcher menu, taskbar buttons, and system tray (clock, memory, uptime, power menu).

---

## 1. Window System Protocol Specification

### 1.1 Transport & Framing
- **Socket Path**: `/run/display.sock` (AF_UNIX stream socket in tmpfs).
- **Framing Header (12 bytes)**:
```c
struct win_msg_header {
    uint32_t len;        /* Total message size in bytes including this header (<= 4096) */
    uint16_t type;       /* WIN_MSG_* */
    uint16_t flags;      /* Reserved / flags (default 0) */
    uint32_t window_id;  /* Window ID (0 for connection-level messages) */
};
```
- **Byte Order**: Little-endian (32-bit x86 native).
- **Size Bounds**: Minimum size is `sizeof(struct win_msg_header)` (12 bytes). Maximum size is `WIN_MAX_MSG_LEN` (4096 bytes). Any message violating these bounds results in immediate client disconnection.

### 1.2 Message Types

#### Client to Server
| Type | Name | Payload Struct | Description |
|---|---|---|---|
| 1 | `WIN_MSG_HELLO` | `struct win_msg_hello` | Initial handshake with protocol version and client PID. |
| 2 | `WIN_MSG_CREATE_WINDOW` | `struct win_msg_create_window` | Requests creation of a window with initial size, flags, and title. |
| 3 | `WIN_MSG_DESTROY_WINDOW` | None | Destroys the target window and frees associated server resources. |
| 4 | `WIN_MSG_SET_TITLE` | `struct win_msg_set_title` | Updates window title string. |
| 5 | `WIN_MSG_SET_MIN_MAX_SIZE` | `struct win_msg_set_min_max_size` | Configures window size constraints. |
| 6 | `WIN_MSG_ATTACH_BUFFER` | `struct win_msg_attach_buffer` | Registers a shared memory buffer index (0 or 1). |
| 7 | `WIN_MSG_DAMAGE` | `struct win_msg_damage` | Marks a window sub-rectangle as dirty. |
| 8 | `WIN_MSG_COMMIT` | `struct win_msg_commit` | Atomically commits a buffer index for composition. |
| 9 | `WIN_MSG_CONFIGURE_ACK` | None | Acknowledges resize/reallocation from `WIN_MSG_CONFIGURE`. |
| 10 | `WIN_MSG_CLIPBOARD_SET` | `struct win_msg_clipboard_set` | Sets global clipboard data and mime type. |
| 11 | `WIN_MSG_CLIPBOARD_GET` | `struct win_msg_clipboard_get` | Requests current clipboard data. |
| 12 | `WIN_MSG_REQUEST_MOVE` | None | Requests server interactive drag-to-move for this window. |
| 13 | `WIN_MSG_SET_STATE` | `struct win_msg_set_state` | Sets window state (normal, maximize, minimize, fullscreen). |

#### Control (Client to Server)
| Type | Name | Payload Struct | Description |
|---|---|---|---|
| 50 | `WIN_MSG_CTRL_HELLO` | None | Identifies connection as a control client (`wmctl`). |
| 51 | `WIN_MSG_CTRL_LIST` | None | Queries list of all active windows. |
| 52 | `WIN_MSG_CTRL_ACTION` | `struct win_msg_ctrl_action` | Requests window management action (focus, close, move, etc.). |
| 53 | `WIN_MSG_CTRL_STATS` | None | Requests compositor frame and performance metrics. |
| 54 | `WIN_MSG_CTRL_SCREENSHOT` | `struct win_msg_ctrl_screenshot` | Saves complete screen dump to specified BMP path. |

#### Server to Client
| Type | Name | Payload Struct | Description |
|---|---|---|---|
| 100 | `WIN_MSG_HELLO_ACK` | `struct win_msg_hello_ack` | Handshake response with display resolution (`screen_w`, `screen_h`). |
| 101 | `WIN_MSG_CONFIGURE` | `struct win_msg_configure` | Notifies client of new dimensions and window state. |
| 102 | `WIN_MSG_KEY` | `struct win_msg_key` | Dispatches translated keycode, Unicode char, modifiers, and state. |
| 103 | `WIN_MSG_POINTER_ENTER` | `struct win_msg_pointer_enter` | Pointer entered window bounds (window-relative coordinates). |
| 104 | `WIN_MSG_POINTER_LEAVE` | None | Pointer exited window bounds. |
| 105 | `WIN_MSG_POINTER_MOVE` | `struct win_msg_pointer_move` | Pointer moved inside window (window-relative coordinates). |
| 106 | `WIN_MSG_POINTER_BUTTON` | `struct win_msg_pointer_button` | Mouse button press or release event. |
| 107 | `WIN_MSG_POINTER_WHEEL` | `struct win_msg_pointer_wheel` | Mouse scroll wheel delta. |
| 108 | `WIN_MSG_FOCUS` | `struct win_msg_focus` | Window gained (`focused=1`) or lost (`focused=0`) keyboard focus. |
| 109 | `WIN_MSG_CLOSE_REQUEST` | None | User clicked close button or requested window close (Alt+F4). |
| 110 | `WIN_MSG_BUFFER_RELEASE` | `struct win_msg_buffer_release` | Notifies client that previous buffer is no longer read by compositor. |
| 111 | `WIN_MSG_FRAME_DONE` | None | Compositor completed frame composition; pacing signal. |
| 112 | `WIN_MSG_CLIPBOARD_DATA` | `struct win_msg_clipboard_data` | Delivers clipboard payload. |

#### Control (Server to Client)
| Type | Name | Payload Struct | Description |
|---|---|---|---|
| 200 | `WIN_MSG_CTRL_ACK` | None | Control command succeeded. |
| 201 | `WIN_MSG_CTRL_LIST_ENTRY` | `struct win_msg_ctrl_list_entry` | Single window metadata record (id, title, pid, bounds, state, focus). |
| 202 | `WIN_MSG_CTRL_STATS_DATA` | `struct win_msg_ctrl_stats_data` | Performance metrics (composite count, average/max ms, flushed bytes). |

---

## 2. Shared Memory Buffer Management & Fault Isolation

### 2.1 Buffer Creation & Attachment
1. Client creates two shared memory files:
   `/dev/shm/win-<pid>-<wid>-0` and `/dev/shm/win-<pid>-<wid>-1`.
2. Client sizes files via `ftruncate(fd, stride * height)` and maps them with `mmap(..., PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`.
3. Client wraps mapped pixels in a `gfx_surface_t` structure.
4. Client sends `WIN_MSG_ATTACH_BUFFER` for buffer 0 and buffer 1 with:
   - `shm_name`: filename string (e.g. `win-12-1-0`).
   - `width`, `height`, `stride`, `format` (`WIN_FORMAT_ARGB8888` or `WIN_FORMAT_XRGB8888`).
5. Server performs strict validation:
   - Name must not contain `/`, must consist solely of `[a-zA-Z0-9_\-\.]`, length <= 63.
   - `width` and `height` within `[1, 4096]`.
   - `stride >= width * 4`.
   - Required size `req_size = stride * height <= 64 MB`.
   - Opens `/dev/shm/<name>` with `O_RDONLY`.
   - Executes `fstat(fd, &st)` and verifies `st.st_size >= req_size`.
   - Maps buffer into compositor address space using `mmap(..., PROT_READ, MAP_SHARED, fd, 0)`.

### 2.2 Truncation & SIGBUS Defense
If a malicious or buggy client truncates its shared memory file after attaching (`ftruncate(fd, 0)`), any page fault attempting to read client pixels raises `SIGBUS`. To prevent compositor termination:
1. At every `WIN_MSG_COMMIT`, the server re-issues `fstat()` on the buffer file descriptor to check current file size against `stride * height`.
2. A dedicated `SIGBUS` signal handler is installed in the compositor using `sigaction`.
3. During client buffer blitting into the compositor RAM backbuffer, the operation is protected by `sigsetjmp(client_bus_jmp, 1)`:
   - If `SIGBUS` occurs, the signal handler invokes `siglongjmp(client_bus_jmp, 1)`.
   - The compositor catches the fault, logs an error to `/run/wm.log`, immediately frees and unmaps the client's buffers, and disconnects that client without crashing.

---

## 3. Client Library (`libwin`) API

### 3.1 Connection & Window Lifecycle
```c
win_conn_t* win_connect(void);
void        win_disconnect(win_conn_t* conn);
int         win_fd(win_conn_t* conn);

win_t*      win_create(win_conn_t* conn, int width, int height, uint32_t flags, const char* title);
void        win_destroy(win_t* win);
void        win_set_title(win_t* win, const char* title);
void        win_set_min_max_size(win_t* win, int min_w, int min_h, int max_w, int max_h);
void        win_set_state(win_t* win, uint32_t state);
void        win_request_move(win_t* win);
```

### 3.2 Drawing & Double Buffering
```c
gfx_surface_t* win_buffer(win_t* win);
void           win_damage(win_t* win, int x, int y, int w, int h);
void           win_commit(win_t* win);
```
- `win_buffer()` returns a `gfx_surface_t*` pointing to the current back buffer.
- `win_damage()` marks the modified area.
- `win_commit()` swaps the front and back buffer indices and notifies the compositor.

### 3.3 Event Handling
```c
typedef struct {
    uint32_t type; /* WIN_EVENT_KEY, WIN_EVENT_POINTER_MOVE, etc. */
    win_t*   window;
    union {
        struct { uint32_t keycode, unicode, modifiers, action; } key;
        struct { int32_t x, y, dx, dy; uint32_t button, buttons, wheel; } pointer;
        struct { uint32_t width, height, state; } configure;
        struct { uint32_t focused; } focus;
    };
} win_event_t;

int win_event_wait(win_conn_t* conn, win_event_t* event, int timeout_ms);
int win_event_poll(win_conn_t* conn, win_event_t* event);
```

### 3.4 Clipboard Helpers
```c
void win_clipboard_set(win_conn_t* conn, const char* mime, const void* data, uint32_t len);
int  win_clipboard_get(win_conn_t* conn, const char* mime, void* buf, uint32_t max_len);
```

---

## 4. Compositor Architecture (`wm`)

### 4.1 Display Management & Backbuffer Composition
- Upon startup, `wm` calls `gfx_open()` to switch `/dev/console` into `KD_GRAPHICS` and `K_OFF`, maps `/dev/fb0`, and queries display geometry (1024x768x32).
- Maintains a 32bpp RAM back buffer of matching dimensions.
- Composes active windows in z-order over desktop background color (`#2A4D69`).
- Tracks screen dirty regions (`dirty_rect_t` merged rectangles).
- Flushes only dirty rectangles to `/dev/fb0` using high-speed `memcpy`/`rep movsd`, never reading from the linear framebuffer.
- Blits a software cursor at mouse position before flipping dirty regions.

### 4.2 Keymap Translation (`/usr/share/keymaps/us.map`)
- Keyboard events from `/dev/input/kbd0` produce Linux scancodes.
- Compositor loads `/usr/share/keymaps/us.map` mapping keycodes to four columns:
  `<keycode> <normal_unicode> <shift_unicode> <caps_unicode> <altgr_unicode>`.
- Generates `WIN_MSG_KEY` messages with exact Unicode codepoints and modifier masks.

---

## 5. Control Tool (`wmctl`) & Configuration

### 5.1 CLI Commands
- `wmctl list`: Lists active windows with IDs, titles, PIDs, geometry, and focus state.
- `wmctl focus <id>`: Gives focus and brings window to top of z-order.
- `wmctl close <id>`: Sends `WIN_MSG_CLOSE_REQUEST` to window client.
- `wmctl move <id> <x> <y>`: Moves window to position.
- `wmctl resize <id> <w> <h>`: Resizes window and triggers `WIN_MSG_CONFIGURE`.
- `wmctl maximize <id>` / `minimize <id>` / `restore <id>`: Adjusts window state.
- `wmctl screenshot <path>`: Captures current screen into BMP format.
- `wmctl stats`: Displays composition metrics, FPS, dirty bytes flushed, and active clients.
- `wmctl quit`: Gracefully shuts down compositor and restores text console.

### 5.2 Desktop Configuration (`/etc/myos/desktop.conf`)
```ini
[desktop]
wallpaper=none
bg_color=0x2A4D69
panel_height=28
theme=dark

[autostart]
app1=/bin/panel.elf
app2=/bin/files.elf
```
