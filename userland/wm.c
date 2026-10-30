/*
 * MyOS Window Manager & Compositor (wm.c) - Stage 18B-2 Step 2
 *
 * Implements:
 * - AF_UNIX stream socket display server at /run/display.sock
 * - AF_UNIX stream socket control server at /run/wm.sock (for wmctl)
 * - Server-side window decorations:
 *     * 24px titlebar with left-aligned title text truncated with "..."
 *     * 3px borders with active/inactive coloring
 *     * Titlebar buttons: Close (red hover), Maximize/Restore, Minimize
 *     * 4px semi-transparent drop shadow
 * - Interactive window management:
 *     * Titlebar click-and-drag window movement clamped to screen
 *     * Border/corner click-and-drag window resize with cursor changes
 *     * Z-ordering: click anywhere raises and focuses
 *     * Maximize / Restore and Minimize window states
 * - Global Shortcuts:
 *     * Alt+Tab: modal window switcher overlay cycling windows
 *     * Alt+F4: sends CLOSE_REQUEST to active window
 *     * Super / Win key or Ctrl+Esc: toggle launcher stub
 * - Theme configuration loaded from /etc/myos/desktop.conf (with built-in fallbacks)
 * - Display ownership via libgfx (KD_GRAPHICS + K_OFF)
 * - RAM back buffer composition with dirty rect blitting to /dev/fb0
 * - Keyboard scancode to Unicode translation (/usr/share/keymaps/us.map)
 * - Strict client shm buffer validation and SIGBUS defense
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include <sigsetjmp.h>
#include <time.h>
#include <gfx.h>
#include <win_protocol.h>
#include <agent.h>
#include <approval_protocol.h>

#define WM_CTRL_SOCK_PATH "/run/wm.sock"
#define MAX_CLIENTS 32
#define MAX_CTRL_CLIENTS 8
#define MAX_WINDOWS 64
#define MAX_DIRTY_RECTS 64

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* SHA-256 for audit logging */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} wm_sha256_ctx_t;

#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)     ((x >> 2 | x << 30) ^ (x >> 13 | x << 19) ^ (x >> 22 | x << 10))
#define EP1(x)     ((x >> 6 | x << 26) ^ (x >> 11 | x << 21) ^ (x >> 25 | x << 7))
#define SIG0(x)    ((x >> 7 | x << 25) ^ (x >> 18 | x << 14) ^ (x >> 3))
#define SIG1(x)    ((x >> 17 | x << 15) ^ (x >> 19 | x << 13) ^ (x >> 10))

static const uint32_t s_sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void wm_sha256_transform(wm_sha256_ctx_t* ctx, const uint8_t* data) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (int i = 16; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + s_sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void wm_sha256_init(wm_sha256_ctx_t* ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void wm_sha256_update(wm_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            wm_sha256_transform(ctx, ctx->buffer);
    }
}

static void wm_sha256_final(wm_sha256_ctx_t* ctx, uint8_t* hash) {
    uint8_t pad = 0x80;
    uint64_t total_bits = ctx->count * 8;
    wm_sha256_update(ctx, &pad, 1);
    while ((ctx->count % 64) != 56) {
        pad = 0x00;
        wm_sha256_update(ctx, &pad, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; --i) {
        len_bytes[i] = (uint8_t)(total_bits & 0xff);
        total_bits >>= 8;
    }
    wm_sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            hash[i + (j * 4)] = (uint8_t)((ctx->state[j] >> (24 - i * 8)) & 0x000000ff);
        }
    }
}

static void wm_hash_to_hex(const uint8_t* hash, char* out, size_t out_cap) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16 && (size_t)(i * 2 + 2) < out_cap; i++) {
        out[i * 2]     = hex[(hash[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[hash[i] & 0xF];
    }
    out[32 < out_cap ? 32 : out_cap - 1] = '\0';
}

static const uint32_t s_agent_colors[] = {
    0xFF3B82F6, /* 1: Blue */
    0xFF10B981, /* 2: Emerald */
    0xFFF59E0B, /* 3: Amber */
    0xFF8B5CF6, /* 4: Purple */
    0xFFEC4899, /* 5: Pink */
    0xFF06B6D4  /* 6: Cyan */
};

static void detect_client_app_name(pid_t pid, char* out_name, size_t cap) {
    out_name[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[128];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char* nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            char* p = strrchr(buf, '/');
            const char* base = p ? p + 1 : buf;
            char* dot = strstr(base, ".elf");
            if (dot) *dot = '\0';
            strncpy(out_name, base, cap - 1);
            out_name[cap - 1] = '\0';
            return;
        }
    }
    snprintf(out_name, cap, "proc-%d", (int)pid);
}

static int check_agent_window_cap(uint32_t agent_id, uint32_t cap_flag, const char* app_name) {
    if (agent_id == 0) return 1; /* Unconfined non-agent process */

    agent_info_t info;
    if (agent_info((int)agent_id, &info) != 0) return 0;

    if (!(info.caps.mask & cap_flag)) return 0;

    if (cap_flag == CAP_WINDOW_OBSERVE) {
        if (info.caps.num_window_observe == 0) return 1;
        for (uint32_t i = 0; i < info.caps.num_window_observe && i < MAX_PATH_RULES; i++) {
            const char* pfx = info.caps.window_observe[i].prefix;
            if (strcmp(pfx, "*") == 0 || strcmp(pfx, app_name) == 0 || strstr(app_name, pfx) != NULL) {
                return 1;
            }
        }
        return 0;
    }

    if (cap_flag == CAP_WINDOW_ACT) {
        if (info.caps.num_window_act == 0) return 1;
        for (uint32_t i = 0; i < info.caps.num_window_act && i < MAX_PATH_RULES; i++) {
            const char* pfx = info.caps.window_act[i].prefix;
            if (strcmp(pfx, "*") == 0 || strcmp(pfx, app_name) == 0 || strstr(app_name, pfx) != NULL) {
                return 1;
            }
        }
        return 0;
    }

    return 1;
}

struct approval_state {
    int active;
    uint32_t approval_id;
    uint32_t agent_id;
    char agent_name[32];
    char skill[64];
    char args_summary[128];
    char args_hash[64];
    uint64_t expires_ms;
    int focused_btn; /* 0 = Deny, 1 = Allow Once, 2 = Allow Session */
    int intentd_fd;
};
static struct approval_state g_approval;
static int g_approval_srv_fd = -1;
static int g_approval_cl_fd = -1;
static void wm_resolve_approval(int status);

#define MAX_PENDING_A11Y_ACTIONS 8
struct pending_a11y_action {
    int active;
    uint64_t resume_time;
    int requester_client_idx;
    uint32_t window_id;
    uint32_t caller_agent_id;
    char expected_app_name[32];
    struct win_msg_a11y_action act;
};
static struct pending_a11y_action g_pending_a11y[MAX_PENDING_A11Y_ACTIONS];

/* Drag Modes */
enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE
};

/* Hit Test Zones */
enum {
    HIT_NONE = 0,
    HIT_CLIENT,
    HIT_TITLEBAR,
    HIT_CLOSE_BTN,
    HIT_MAX_BTN,
    HIT_MIN_BTN,
    HIT_BORDER_W,
    HIT_BORDER_E,
    HIT_BORDER_N,
    HIT_BORDER_S,
    HIT_CORNER_NW,
    HIT_CORNER_NE,
    HIT_CORNER_SW,
    HIT_CORNER_SE
};

/* Cursor Shapes */
enum {
    CURSOR_ARROW = 0,
    CURSOR_RESIZE_EW,
    CURSOR_RESIZE_NS,
    CURSOR_RESIZE_NWSE,
    CURSOR_RESIZE_NESW
};

struct rect {
    int x, y, w, h;
};

struct desktop_theme {
    uint32_t active_titlebar;
    uint32_t inactive_titlebar;
    uint32_t titlebar_text;
    uint32_t active_border;
    uint32_t inactive_border;
    int titlebar_height;
    int border_width;
    char keymap_path[128];
};

struct client_buffer {
    char shm_name[64];
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    size_t size;
    int fd;
    const uint32_t* pixels;
    int valid;
};

struct server_window {
    uint32_t id;
    int client_idx;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t min_w, min_h;
    uint32_t max_w, max_h;
    uint32_t flags;
    uint32_t state;
    char title[128];

    pid_t pid;
    uint32_t agent_id;
    char app_name[64];

    /* Agent control indicators */
    uint32_t agent_control_id;
    char agent_control_name[32];
    uint64_t agent_control_expire_ms;
    int target_indicator_active;
    int target_x, target_y, target_w, target_h;

    /* Collision detection with real hardware input */
    uint64_t last_hardware_input_time;

    /* A11Y request routing: client slot waiting for reply */
    int a11y_requester_client_idx;

    /* Saved geometry for maximize / restore */
    int32_t saved_x;
    int32_t saved_y;
    uint32_t saved_w;
    uint32_t saved_h;

    /* Decoration button state (1=close, 2=max, 3=min) */
    int button_hover;

    struct client_buffer buffers[2];
    int active_buffer; /* 0 or 1, or -1 if none */

    int alive;
};

struct client {
    int fd;
    pid_t pid;
    uint32_t agent_id;
    int is_privileged;
    char app_name[64];
    int active;

    uint8_t rx_buf[WIN_MAX_MSG_LEN * 2];
    size_t rx_len;
};

struct ctrl_client {
    int fd;
    int active;
    char rx_buf[512];
    size_t rx_len;
};

/* Global Compositor State */
static gfx_context_t* ctx = NULL;
static int screen_w = 1024;
static int screen_h = 768;
static int mouse_x = 512;
static int mouse_y = 384;
static int prev_mouse_x = 512;
static int prev_mouse_y = 384;
static int mouse_buttons = 0;
static int g_current_cursor = CURSOR_ARROW;

static struct desktop_theme g_theme;
static struct client clients[MAX_CLIENTS];
static struct ctrl_client ctrl_clients[MAX_CTRL_CLIENTS];
static struct server_window windows[MAX_WINDOWS];
static uint32_t z_order[MAX_WINDOWS];
static int num_z_windows = 0;
static uint32_t focused_window_id = 0;
static int hovered_window_idx = -1;

/* Interactive Drag & Resize State */
static int g_drag_state = DRAG_NONE;
static uint32_t g_drag_win_id = 0;
static int g_drag_hit_zone = HIT_NONE;
static int g_drag_offset_x = 0;
static int g_drag_offset_y = 0;
static int g_drag_start_x = 0;
static int g_drag_start_y = 0;
static int g_drag_start_w = 0;
static int g_drag_start_h = 0;
static int g_drag_start_mx = 0;
static int g_drag_start_my = 0;

/* Alt+Tab Window Switcher State */
static int g_switcher_active = 0;
static int g_switcher_idx = 0;
static uint32_t g_switcher_list[MAX_WINDOWS];
static int g_switcher_count = 0;
static uint32_t g_last_mods = 0;

/* Dirty Rects */
static struct rect dirty_rects[MAX_DIRTY_RECTS];
static int num_dirty_rects = 0;

/* Clipboard */
static char g_clipboard_mime[32];
static char g_clipboard_data[1024];
static uint32_t g_clipboard_len = 0;

/* SIGBUS & Termination */
static sigjmp_buf g_bus_jmp;
static volatile sig_atomic_t g_in_client_blit = 0;
static int g_faulting_client = -1;
static volatile sig_atomic_t g_running = 1;

/* Keymap */
struct keymap_entry {
    uint32_t normal;
    uint32_t shift;
    uint32_t caps;
    uint32_t altgr;
};
static struct keymap_entry g_keymap[256];

/* Cursor Bitmaps */
static const char* s_cursor_arrow[16] = {
    "X               ",
    "XX              ",
    "X.X             ",
    "X..X            ",
    "X...X           ",
    "X....X          ",
    "X.....X         ",
    "X......X        ",
    "X.......X       ",
    "X.....XXXX      ",
    "X..X..X         ",
    "X.X X..X        ",
    "XX   X..X       ",
    "X     X..X      ",
    "       XX       ",
    "                "
};

static const char* s_cursor_ew[16] = {
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "    X      X    ",
    "   XX      XX   ",
    "  X..XXXXXX..X  ",
    " X............X ",
    "  X..XXXXXX..X  ",
    "   XX      XX   ",
    "    X      X    ",
    "                ",
    "                ",
    "                ",
    "                "
};

static const char* s_cursor_ns[16] = {
    "       XX       ",
    "      X..X      ",
    "     X....X     ",
    "    X......X    ",
    "    XXXXXX      ",
    "       X.X      ",
    "       X.X      ",
    "       X.X      ",
    "       X.X      ",
    "       X.X      ",
    "       X.X      ",
    "    XXXXXX      ",
    "    X......X    ",
    "     X....X     ",
    "      X..X      ",
    "       XX       "
};

static const char* s_cursor_nwse[16] = {
    "XXXXX           ",
    "X...XX          ",
    "X....XX         ",
    "X..X..XX        ",
    "XX  X..XX       ",
    "     X..XX      ",
    "      X..XX     ",
    "       X..XX    ",
    "        X..XX   ",
    "         X..XX  ",
    "       XX..X  XX",
    "        XX..X..X",
    "         XX....X",
    "          XX...X",
    "           XXXXX",
    "                "
};

static const char* s_cursor_nesw[16] = {
    "           XXXXX",
    "          XX...X",
    "         XX....X",
    "        XX..X..X",
    "       XX..X  XX",
    "      XX..X     ",
    "     XX..X      ",
    "    XX..X       ",
    "   XX..X        ",
    "  XX..X         ",
    "XX  X..XX       ",
    "X..X..XX        ",
    "X....XX         ",
    "X...XX          ",
    "XXXXX           ",
    "                "
};

/* Forward Declarations */
static void add_dirty_rect(int x, int y, int w, int h);
static struct server_window* find_window_by_id(uint32_t wid);
static void get_window_frame(const struct server_window* win, int* fx, int* fy, int* fw, int* fh);
static void invalidate_window_frame(const struct server_window* win);
static void set_focused_window(uint32_t wid);
static void raise_window(uint32_t wid);
static void toggle_maximize(struct server_window* win);
static int send_client_msg(int fd, uint16_t type, uint32_t window_id, const void* payload, uint32_t payload_len);
static void disconnect_client(int c_idx, const char* reason);

static void sigbus_handler(int sig) {
    (void) sig;
    if (g_in_client_blit) {
        siglongjmp(g_bus_jmp, 1);
    }
    _exit(135);
}

static void sigterm_handler(int sig) {
    (void) sig;
    g_running = 0;
}

/* Theme Configuration Parsing */
static uint32_t parse_color_hex(const char* hex, uint32_t fallback) {
    if (!hex) return fallback;
    while (*hex == ' ' || *hex == '\t') hex++;
    if (*hex == '#') hex++;
    if (strlen(hex) < 6) return fallback;
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    return fallback;
}

static void load_desktop_config(const char* path) {
    /* Sane defaults */
    g_theme.active_titlebar   = 0xFF1E3A8A; /* Deep blue */
    g_theme.inactive_titlebar = 0xFF475569; /* Slate gray */
    g_theme.titlebar_text     = 0xFFFFFFFF; /* White */
    g_theme.active_border     = 0xFF3B82F6; /* Vibrant blue */
    g_theme.inactive_border   = 0xFF64748B; /* Muted slate */
    g_theme.titlebar_height   = 24;
    g_theme.border_width      = 3;
    strncpy(g_theme.keymap_path, "/usr/share/keymaps/us.map", sizeof(g_theme.keymap_path) - 1);

    FILE* fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r') continue;

        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;

        /* Trim key */
        char* kend = key + strlen(key) - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

        /* Trim val */
        while (*val == ' ' || *val == '\t') val++;
        char* vend = val + strlen(val) - 1;
        while (vend >= val && (*vend == ' ' || *vend == '\t' || *vend == '\n' || *vend == '\r')) *vend-- = '\0';

        if (strcmp(key, "active_titlebar") == 0) {
            g_theme.active_titlebar = parse_color_hex(val, g_theme.active_titlebar);
        } else if (strcmp(key, "inactive_titlebar") == 0) {
            g_theme.inactive_titlebar = parse_color_hex(val, g_theme.inactive_titlebar);
        } else if (strcmp(key, "titlebar_text") == 0) {
            g_theme.titlebar_text = parse_color_hex(val, g_theme.titlebar_text);
        } else if (strcmp(key, "active_border") == 0) {
            g_theme.active_border = parse_color_hex(val, g_theme.active_border);
        } else if (strcmp(key, "inactive_border") == 0) {
            g_theme.inactive_border = parse_color_hex(val, g_theme.inactive_border);
        } else if (strcmp(key, "titlebar_height") == 0) {
            int h = atoi(val);
            if (h >= 16 && h <= 48) g_theme.titlebar_height = h;
        } else if (strcmp(key, "border_width") == 0) {
            int b = atoi(val);
            if (b >= 1 && b <= 10) g_theme.border_width = b;
        } else if (strcmp(key, "keymap") == 0) {
            strncpy(g_theme.keymap_path, val, sizeof(g_theme.keymap_path) - 1);
        }
    }
    fclose(fp);
}

static void init_fallback_keymap(void) {
    memset(g_keymap, 0, sizeof(g_keymap));

    const char* num_norm = "1234567890-=";
    const char* num_shft = "!@#$%^&*()_+";
    for (int i = 0; i < 12; i++) {
        int code = KEY_1 + i;
        g_keymap[code].normal = (unsigned char)num_norm[i];
        g_keymap[code].shift  = (unsigned char)num_shft[i];
        g_keymap[code].caps   = (unsigned char)num_norm[i];
        g_keymap[code].altgr  = (unsigned char)num_norm[i];
    }

    g_keymap[KEY_ESC].normal = 0x1b;
    g_keymap[KEY_ESC].shift  = 0x1b;
    g_keymap[KEY_BACKSPACE].normal = 0x08;
    g_keymap[KEY_BACKSPACE].shift  = 0x08;
    g_keymap[KEY_TAB].normal = 0x09;
    g_keymap[KEY_TAB].shift  = 0x09;
    g_keymap[KEY_ENTER].normal = 0x0a;
    g_keymap[KEY_ENTER].shift  = 0x0a;
    g_keymap[KEY_SPACE].normal = ' ';
    g_keymap[KEY_SPACE].shift  = ' ';

    static const struct { int code; char lower; char upper; } letters[] = {
        { KEY_Q, 'q', 'Q' }, { KEY_W, 'w', 'W' }, { KEY_E, 'e', 'E' },
        { KEY_R, 'r', 'R' }, { KEY_T, 't', 'T' }, { KEY_Y, 'y', 'Y' },
        { KEY_U, 'u', 'U' }, { KEY_I, 'i', 'I' }, { KEY_O, 'o', 'O' },
        { KEY_P, 'p', 'P' }, { KEY_A, 'a', 'A' }, { KEY_S, 's', 'S' },
        { KEY_D, 'd', 'D' }, { KEY_F, 'f', 'F' }, { KEY_G, 'g', 'G' },
        { KEY_H, 'h', 'H' }, { KEY_J, 'j', 'J' }, { KEY_K, 'k', 'K' },
        { KEY_L, 'l', 'L' }, { KEY_Z, 'z', 'Z' }, { KEY_X, 'x', 'X' },
        { KEY_C, 'c', 'C' }, { KEY_V, 'v', 'V' }, { KEY_B, 'b', 'B' },
        { KEY_N, 'n', 'N' }, { KEY_M, 'm', 'M' }
    };
    for (size_t i = 0; i < sizeof(letters)/sizeof(letters[0]); i++) {
        int c = letters[i].code;
        g_keymap[c].normal = (unsigned char)letters[i].lower;
        g_keymap[c].shift  = (unsigned char)letters[i].upper;
        g_keymap[c].caps   = (unsigned char)letters[i].upper;
        g_keymap[c].altgr  = (unsigned char)letters[i].lower;
    }

    g_keymap[KEY_LEFTBRACE].normal = '['; g_keymap[KEY_LEFTBRACE].shift = '{';
    g_keymap[KEY_RIGHTBRACE].normal = ']'; g_keymap[KEY_RIGHTBRACE].shift = '}';
    g_keymap[KEY_SEMICOLON].normal = ';'; g_keymap[KEY_SEMICOLON].shift = ':';
    g_keymap[KEY_APOSTROPHE].normal = '\''; g_keymap[KEY_APOSTROPHE].shift = '"';
    g_keymap[KEY_GRAVE].normal = '`'; g_keymap[KEY_GRAVE].shift = '~';
    g_keymap[KEY_BACKSLASH].normal = '\\'; g_keymap[KEY_BACKSLASH].shift = '|';
    g_keymap[KEY_COMMA].normal = ','; g_keymap[KEY_COMMA].shift = '<';
    g_keymap[KEY_DOT].normal = '.'; g_keymap[KEY_DOT].shift = '>';
    g_keymap[KEY_SLASH].normal = '/'; g_keymap[KEY_SLASH].shift = '?';
}

static void load_keymap(const char* path) {
    init_fallback_keymap();
    FILE* fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        int code = 0;
        unsigned int norm = 0, shft = 0, caps = 0, alt = 0;
        if (sscanf(line, "%d %x %x %x %x", &code, &norm, &shft, &caps, &alt) >= 2) {
            if (code >= 0 && code < 256) {
                g_keymap[code].normal = norm;
                g_keymap[code].shift  = (shft != 0) ? shft : norm;
                g_keymap[code].caps   = (caps != 0) ? caps : norm;
                g_keymap[code].altgr  = (alt != 0)  ? alt  : norm;
            }
        }
    }
    fclose(fp);
}

static uint32_t translate_key(uint32_t keycode, uint32_t modifiers) {
    if (keycode >= 256) return 0;
    int shift = (modifiers & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
    int caps  = (modifiers & KBD_MOD_CAPS) != 0;
    int altgr = (modifiers & KBD_MOD_RALT) != 0;

    if (altgr && g_keymap[keycode].altgr) return g_keymap[keycode].altgr;
    if (caps && !shift && g_keymap[keycode].caps) return g_keymap[keycode].caps;
    if (shift && g_keymap[keycode].shift) return g_keymap[keycode].shift;
    return g_keymap[keycode].normal;
}

static void add_dirty_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;

    if (num_dirty_rects < MAX_DIRTY_RECTS) {
        dirty_rects[num_dirty_rects].x = x;
        dirty_rects[num_dirty_rects].y = y;
        dirty_rects[num_dirty_rects].w = w;
        dirty_rects[num_dirty_rects].h = h;
        num_dirty_rects++;
    } else {
        dirty_rects[0].x = 0;
        dirty_rects[0].y = 0;
        dirty_rects[0].w = screen_w;
        dirty_rects[0].h = screen_h;
        num_dirty_rects = 1;
    }
}

static int send_client_msg(int fd, uint16_t type, uint32_t window_id, const void* payload, uint32_t payload_len) {
    uint32_t total_len = sizeof(struct win_msg_header) + payload_len;
    struct win_msg_header hdr;
    hdr.len = total_len;
    hdr.type = type;
    hdr.flags = 0;
    hdr.window_id = window_id;

    uint8_t buf[WIN_MAX_MSG_LEN];
    memcpy(buf, &hdr, sizeof(hdr));
    if (payload && payload_len > 0) {
        memcpy(buf + sizeof(hdr), payload, payload_len);
    }

    size_t written = 0;
    while (written < total_len) {
        int nw = write(fd, buf + written, total_len - written);
        if (nw <= 0) {
            if (nw < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        written += nw;
    }
    return 0;
}

static struct server_window* find_window_by_id(uint32_t wid) {
    if (wid == 0) return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].alive && windows[i].id == wid) {
            return &windows[i];
        }
    }
    return NULL;
}

static void get_window_frame(const struct server_window* win, int* fx, int* fy, int* fw, int* fh) {
    if (win->flags & WIN_FLAG_BORDERLESS) {
        *fx = win->x;
        *fy = win->y;
        *fw = (int)win->width;
        *fh = (int)win->height;
    } else {
        int bw = g_theme.border_width;
        int th = g_theme.titlebar_height;
        *fx = win->x - bw;
        *fy = win->y - th;
        *fw = (int)win->width + 2 * bw;
        *fh = (int)win->height + th + bw;
    }
}

static void invalidate_window_frame(const struct server_window* win) {
    int fx, fy, fw, fh;
    get_window_frame(win, &fx, &fy, &fw, &fh);
    /* Invalidate frame and drop shadow */
    add_dirty_rect(fx - 4, fy - 4, fw + 14, fh + 14);
}

static void raise_window(uint32_t wid) {
    int idx = -1;
    for (int i = 0; i < num_z_windows; i++) {
        if (z_order[i] == wid) {
            idx = i;
            break;
        }
    }
    if (idx >= 0 && idx < num_z_windows - 1) {
        for (int i = idx; i < num_z_windows - 1; i++) {
            z_order[i] = z_order[i + 1];
        }
        z_order[num_z_windows - 1] = wid;
    }
}

static void set_focused_window(uint32_t wid) {
    if (focused_window_id == wid) return;

    if (focused_window_id != 0) {
        struct server_window* old_win = find_window_by_id(focused_window_id);
        if (old_win && old_win->alive) {
            struct win_msg_focus foc = { .focused = 0 };
            send_client_msg(clients[old_win->client_idx].fd, WIN_MSG_FOCUS, old_win->id, &foc, sizeof(foc));
            invalidate_window_frame(old_win);
        }
    }

    focused_window_id = wid;

    if (focused_window_id != 0) {
        struct server_window* new_win = find_window_by_id(focused_window_id);
        if (new_win && new_win->alive) {
            raise_window(wid);
            struct win_msg_focus foc = { .focused = 1 };
            send_client_msg(clients[new_win->client_idx].fd, WIN_MSG_FOCUS, new_win->id, &foc, sizeof(foc));
            invalidate_window_frame(new_win);
        }
    }
}

static void toggle_maximize(struct server_window* win) {
    if (!win || !win->alive) return;
    int bw = g_theme.border_width;
    int th = g_theme.titlebar_height;

    invalidate_window_frame(win);

    if (win->state == WIN_STATE_MAXIMIZED) {
        win->state = WIN_STATE_NORMAL;
        win->x = win->saved_x;
        win->y = win->saved_y;
        win->width = win->saved_w;
        win->height = win->saved_h;
    } else {
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->width;
        win->saved_h = win->height;
        win->state = WIN_STATE_MAXIMIZED;

        win->x = bw;
        win->y = th;
        win->width = screen_w - 2 * bw;
        win->height = screen_h - th - bw;
    }

    struct win_msg_configure cfg;
    cfg.width = win->width;
    cfg.height = win->height;
    cfg.state = win->state;
    send_client_msg(clients[win->client_idx].fd, WIN_MSG_CONFIGURE, win->id, &cfg, sizeof(cfg));

    invalidate_window_frame(win);
}

static void free_window_buffers(struct server_window* win) {
    for (int b = 0; b < 2; b++) {
        struct client_buffer* buf = &win->buffers[b];
        if (buf->pixels && buf->size > 0) {
            munmap((void*)buf->pixels, buf->size);
            buf->pixels = NULL;
        }
        if (buf->fd >= 0) {
            close(buf->fd);
            buf->fd = -1;
        }
        buf->valid = 0;
        buf->shm_name[0] = '\0';
    }
    win->active_buffer = -1;
}

static void remove_window(int win_idx) {
    struct server_window* win = &windows[win_idx];
    if (!win->alive) return;

    invalidate_window_frame(win);
    free_window_buffers(win);
    win->alive = 0;

    int z_idx = -1;
    for (int i = 0; i < num_z_windows; i++) {
        if (z_order[i] == win->id) {
            z_idx = i;
            break;
        }
    }
    if (z_idx >= 0) {
        for (int i = z_idx; i < num_z_windows - 1; i++) {
            z_order[i] = z_order[i + 1];
        }
        num_z_windows--;
    }

    if (focused_window_id == win->id) {
        focused_window_id = 0;
        for (int i = num_z_windows - 1; i >= 0; i--) {
            struct server_window* cand = find_window_by_id(z_order[i]);
            if (cand && cand->alive && cand->state != WIN_STATE_MINIMIZED) {
                set_focused_window(cand->id);
                break;
            }
        }
    }
}

static void disconnect_client(int c_idx, const char* reason) {
    struct client* c = &clients[c_idx];
    if (!c->active) return;

    if (reason) {
        fprintf(stderr, "[wm] Disconnecting client %d (PID %d): %s\n", c_idx, (int)c->pid, reason);
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].alive && windows[i].client_idx == c_idx) {
            remove_window(i);
        }
    }

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->active = 0;
    c->rx_len = 0;
}

static int validate_shm_name(const char* name) {
    if (!name || !name[0] || strlen(name) > 63) return 0;
    for (int i = 0; name[i]; i++) {
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return 0;
        }
    }
    return 1;
}

static void wm_process_pending_a11y_actions(void) {
    uint64_t now = get_now_ms();
    for (int p = 0; p < MAX_PENDING_A11Y_ACTIONS; p++) {
        if (!g_pending_a11y[p].active) continue;
        if (now < g_pending_a11y[p].resume_time) continue;

        /* Re-resolve target window */
        struct server_window* win = find_window_by_id(g_pending_a11y[p].window_id);
        int req_fd = (g_pending_a11y[p].requester_client_idx >= 0 &&
                      g_pending_a11y[p].requester_client_idx < MAX_CLIENTS &&
                      clients[g_pending_a11y[p].requester_client_idx].active) ?
                      clients[g_pending_a11y[p].requester_client_idx].fd : -1;

        if (!win || !win->alive) {
            if (req_fd >= 0) {
                struct win_msg_a11y_result res;
                memset(&res, 0, sizeof(res));
                res.ok = 0;
                snprintf(res.error, sizeof(res.error), "Aborted: window closed during human input pause");
                send_client_msg(req_fd, WIN_MSG_A11Y_RESULT, g_pending_a11y[p].window_id, &res, sizeof(res));
            }
            g_pending_a11y[p].active = 0;
            continue;
        }

        /* Check if window was replaced / app changed */
        if (strncmp(win->app_name, g_pending_a11y[p].expected_app_name, sizeof(win->app_name)) != 0) {
            if (req_fd >= 0) {
                struct win_msg_a11y_result res;
                memset(&res, 0, sizeof(res));
                res.ok = 0;
                snprintf(res.error, sizeof(res.error), "Aborted: window replaced during human input pause");
                send_client_msg(req_fd, WIN_MSG_A11Y_RESULT, win->id, &res, sizeof(res));
            }
            g_pending_a11y[p].active = 0;
            continue;
        }

        /* Re-verify capability */
        uint32_t caller_agent = g_pending_a11y[p].caller_agent_id;
        if (caller_agent > 0) {
            if (!check_agent_window_cap(caller_agent, CAP_WINDOW_ACT, win->app_name)) {
                if (req_fd >= 0) {
                    struct win_msg_a11y_result res;
                    memset(&res, 0, sizeof(res));
                    res.ok = 0;
                    snprintf(res.error, sizeof(res.error), "Permission denied: lacks CAP_WINDOW_ACT for %s", win->app_name);
                    send_client_msg(req_fd, WIN_MSG_A11Y_RESULT, win->id, &res, sizeof(res));
                }
                g_pending_a11y[p].active = 0;
                continue;
            }
        }

        /* If more human input occurred, push pause further */
        if (now >= win->last_hardware_input_time && (now - win->last_hardware_input_time) < 500) {
            g_pending_a11y[p].resume_time = win->last_hardware_input_time + 500;
            continue;
        }

        /* Visible control indicators */
        win->agent_control_id = caller_agent;
        win->agent_control_expire_ms = now + 1500;
        invalidate_window_frame(win);

        /* Audit log entry with hash of args */
        uint8_t ahash[32];
        wm_sha256_ctx_t hctx;
        wm_sha256_init(&hctx);
        wm_sha256_update(&hctx, (const uint8_t*)g_pending_a11y[p].act.args, strlen(g_pending_a11y[p].act.args));
        wm_sha256_final(&hctx, ahash);
        char hex_hash[33];
        wm_hash_to_hex(ahash, hex_hash, sizeof(hex_hash));

        char audit_detail[96];
        snprintf(audit_detail, sizeof(audit_detail), "agent:%u win:%s act:%s hash:%.16s",
                 (unsigned int)caller_agent, win->app_name, g_pending_a11y[p].act.action, hex_hash);
        audit_append((int)caller_agent, AUDIT_EVENT_WINDOW_ACTION, audit_detail);

        win->a11y_requester_client_idx = g_pending_a11y[p].requester_client_idx;
        send_client_msg(clients[win->client_idx].fd, WIN_MSG_A11Y_ACTION, win->id, &g_pending_a11y[p].act, sizeof(g_pending_a11y[p].act));
        g_pending_a11y[p].active = 0;
    }
}

static void handle_client_message(int c_idx, const struct win_msg_header* hdr, const uint8_t* payload, uint32_t payload_len) {
    struct client* c = &clients[c_idx];

    switch (hdr->type) {
        case WIN_MSG_HELLO: {
            if (payload_len < sizeof(struct win_msg_hello)) {
                disconnect_client(c_idx, "malformed HELLO payload");
                return;
            }
            struct win_msg_hello hello;
            memcpy(&hello, payload, sizeof(hello));
            c->pid = (pid_t) hello.pid;

            struct win_msg_hello_ack ack;
            ack.screen_w = (uint32_t) screen_w;
            ack.screen_h = (uint32_t) screen_h;
            send_client_msg(c->fd, WIN_MSG_HELLO_ACK, 0, &ack, sizeof(ack));
            break;
        }

        case WIN_MSG_CREATE_WINDOW: {
            if (payload_len < sizeof(struct win_msg_create_window)) {
                disconnect_client(c_idx, "malformed CREATE_WINDOW payload");
                return;
            }
            struct win_msg_create_window cw;
            memcpy(&cw, payload, sizeof(cw));

            if (cw.width == 0 || cw.width > 4096 || cw.height == 0 || cw.height > 4096) {
                disconnect_client(c_idx, "window dimensions out of bounds");
                return;
            }

            int slot = -1;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (!windows[i].alive) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                disconnect_client(c_idx, "maximum window limit reached (64)");
                return;
            }

            struct server_window* win = &windows[slot];
            memset(win, 0, sizeof(*win));
            win->id = hdr->window_id;
            win->client_idx = c_idx;
            win->width = cw.width;
            win->height = cw.height;
            win->min_w = 100;
            win->min_h = 60;
            win->max_w = 2048;
            win->max_h = 2048;
            win->flags = cw.flags;
            win->state = WIN_STATE_NORMAL;
            win->pid = clients[c_idx].pid;
            win->agent_id = clients[c_idx].agent_id;
            strncpy(win->app_name, clients[c_idx].app_name, sizeof(win->app_name) - 1);
            win->a11y_requester_client_idx = -1;
            memcpy(win->title, cw.title, sizeof(win->title) - 1);
            win->title[sizeof(win->title) - 1] = '\0';

            static int cascade = 0;
            win->x = (screen_w - (int)win->width) / 2 + (cascade % 8) * 20;
            win->y = (screen_h - (int)win->height) / 2 + (cascade % 8) * 20;
            cascade++;
            if (win->x < g_theme.border_width + 10) win->x = g_theme.border_width + 10;
            if (win->y < g_theme.titlebar_height + 10) win->y = g_theme.titlebar_height + 10;

            for (int b = 0; b < 2; b++) {
                win->buffers[b].fd = -1;
            }
            win->active_buffer = -1;
            win->alive = 1;

            if (num_z_windows < MAX_WINDOWS) {
                z_order[num_z_windows++] = win->id;
            }
            set_focused_window(win->id);
            invalidate_window_frame(win);
            break;
        }

        case WIN_MSG_DESTROY_WINDOW: {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (windows[i].alive && windows[i].id == hdr->window_id && windows[i].client_idx == c_idx) {
                    remove_window(i);
                    break;
                }
            }
            break;
        }

        case WIN_MSG_SET_TITLE: {
            if (payload_len < sizeof(struct win_msg_set_title)) return;
            struct win_msg_set_title st;
            memcpy(&st, payload, sizeof(st));
            struct server_window* win = find_window_by_id(hdr->window_id);
            if (win && win->client_idx == c_idx) {
                strncpy(win->title, st.title, sizeof(win->title) - 1);
                invalidate_window_frame(win);
            }
            break;
        }

        case WIN_MSG_SET_MIN_MAX_SIZE: {
            if (payload_len < sizeof(struct win_msg_set_min_max_size)) return;
            struct win_msg_set_min_max_size mm;
            memcpy(&mm, payload, sizeof(mm));
            struct server_window* win = find_window_by_id(hdr->window_id);
            if (win && win->client_idx == c_idx) {
                if (mm.min_w > 0) win->min_w = mm.min_w;
                if (mm.min_h > 0) win->min_h = mm.min_h;
                if (mm.max_w > 0) win->max_w = mm.max_w;
                if (mm.max_h > 0) win->max_h = mm.max_h;
            }
            break;
        }

        case WIN_MSG_ATTACH_BUFFER: {
            if (payload_len < sizeof(struct win_msg_attach_buffer)) {
                disconnect_client(c_idx, "malformed ATTACH_BUFFER payload");
                return;
            }
            struct win_msg_attach_buffer att;
            memcpy(&att, payload, sizeof(att));

            if (att.buffer_index > 1) {
                disconnect_client(c_idx, "invalid buffer index (must be 0 or 1)");
                return;
            }

            if (!validate_shm_name(att.shm_name)) {
                disconnect_client(c_idx, "invalid shm name charset or length");
                return;
            }

            if (att.width == 0 || att.width > 4096 || att.height == 0 || att.height > 4096 ||
                att.stride < att.width * 4) {
                disconnect_client(c_idx, "invalid buffer dimensions or stride");
                return;
            }

            size_t req_size = (size_t)att.stride * att.height;
            if (req_size > 64 * 1024 * 1024) {
                disconnect_client(c_idx, "buffer exceeds 64MB limit");
                return;
            }

            struct server_window* win = find_window_by_id(hdr->window_id);
            if (!win || win->client_idx != c_idx) {
                disconnect_client(c_idx, "attach buffer target window not found");
                return;
            }

            struct client_buffer* buf = &win->buffers[att.buffer_index];

            if (buf->pixels && buf->size > 0) {
                munmap((void*)buf->pixels, buf->size);
                buf->pixels = NULL;
            }
            if (buf->fd >= 0) {
                close(buf->fd);
                buf->fd = -1;
            }

            char path[128];
            snprintf(path, sizeof(path), "/dev/shm/%s", att.shm_name);

            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                disconnect_client(c_idx, "failed to open client shm file");
                return;
            }

            struct stat st;
            if (fstat(fd, &st) < 0 || (size_t)st.st_size < req_size) {
                close(fd);
                disconnect_client(c_idx, "shm file size less than required stride*height");
                return;
            }

            void* map = mmap(NULL, req_size, PROT_READ, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED || !map) {
                close(fd);
                disconnect_client(c_idx, "mmap client shm buffer failed");
                return;
            }

            memcpy(buf->shm_name, att.shm_name, sizeof(buf->shm_name) - 1);
            buf->shm_name[sizeof(buf->shm_name) - 1] = '\0';
            buf->width = att.width;
            buf->height = att.height;
            buf->stride = att.stride;
            buf->format = att.format;
            buf->size = req_size;
            buf->fd = fd;
            buf->pixels = (const uint32_t*) map;
            buf->valid = 1;
            break;
        }

        case WIN_MSG_DAMAGE: {
            if (payload_len < sizeof(struct win_msg_damage)) return;
            struct win_msg_damage dmg;
            memcpy(&dmg, payload, sizeof(dmg));

            struct server_window* win = find_window_by_id(hdr->window_id);
            if (win && win->client_idx == c_idx && win->state != WIN_STATE_MINIMIZED) {
                add_dirty_rect(win->x + dmg.x, win->y + dmg.y, dmg.width, dmg.height);
            }
            break;
        }

        case WIN_MSG_COMMIT: {
            if (payload_len < sizeof(struct win_msg_commit)) {
                disconnect_client(c_idx, "malformed COMMIT payload");
                return;
            }
            struct win_msg_commit com;
            memcpy(&com, payload, sizeof(com));

            if (com.buffer_index > 1) {
                disconnect_client(c_idx, "commit buffer index out of range");
                return;
            }

            struct server_window* win = find_window_by_id(hdr->window_id);
            if (!win || win->client_idx != c_idx) return;

            struct client_buffer* buf = &win->buffers[com.buffer_index];
            if (!buf->valid || buf->fd < 0) {
                disconnect_client(c_idx, "commit on unattached buffer");
                return;
            }

            struct stat st;
            if (fstat(buf->fd, &st) < 0 || (size_t)st.st_size < buf->size) {
                disconnect_client(c_idx, "shm file truncated at commit");
                return;
            }

            win->active_buffer = com.buffer_index;
            if (win->state != WIN_STATE_MINIMIZED) {
                add_dirty_rect(win->x, win->y, win->width, win->height);
            }

            struct win_msg_buffer_release rel;
            rel.buffer_index = 1 - com.buffer_index;
            send_client_msg(c->fd, WIN_MSG_BUFFER_RELEASE, win->id, &rel, sizeof(rel));
            break;
        }

        case WIN_MSG_CLIPBOARD_SET: {
            if (payload_len < sizeof(struct win_msg_clipboard_set)) return;
            struct win_msg_clipboard_set cs;
            memcpy(&cs, payload, sizeof(cs));
            strncpy(g_clipboard_mime, cs.mime, sizeof(g_clipboard_mime) - 1);
            g_clipboard_len = (cs.len < sizeof(g_clipboard_data)) ? cs.len : sizeof(g_clipboard_data);
            memcpy(g_clipboard_data, cs.data, g_clipboard_len);
            break;
        }

        case WIN_MSG_CLIPBOARD_GET: {
            struct win_msg_clipboard_data cd;
            memset(&cd, 0, sizeof(cd));
            memcpy(cd.mime, g_clipboard_mime, sizeof(cd.mime) - 1);
            cd.mime[sizeof(cd.mime) - 1] = '\0';
            cd.len = g_clipboard_len;
            memcpy(cd.data, g_clipboard_data, g_clipboard_len);
            send_client_msg(c->fd, WIN_MSG_CLIPBOARD_DATA, 0, &cd, sizeof(cd));
            break;
        }

        case WIN_MSG_CONFIGURE_ACK:
            break;

        case WIN_MSG_PRIVILEGED_REGISTER: {
            struct win_msg_privileged_ack ack;
            memset(&ack, 0, sizeof(ack));
            if (c->agent_id == 0) {
                c->is_privileged = 1;
                ack.ok = 1;
            } else {
                ack.ok = 0;
            }
            send_client_msg(c->fd, WIN_MSG_PRIVILEGED_ACK, 0, &ack, sizeof(ack));
            break;
        }

        case WIN_MSG_AGENT_INDICATOR: {
            if (payload_len < sizeof(struct win_msg_agent_indicator)) return;
            struct win_msg_agent_indicator ind;
            memcpy(&ind, payload, sizeof(ind));
            struct server_window* win = find_window_by_id(hdr->window_id);
            if (win) {
                win->agent_control_id = ind.agent_id;
                strncpy(win->agent_control_name, ind.agent_name, sizeof(win->agent_control_name) - 1);
                win->agent_control_expire_ms = get_now_ms() + 1500;
                win->target_indicator_active = 1;
                win->target_x = ind.target_x;
                win->target_y = ind.target_y;
                win->target_w = ind.target_w;
                win->target_h = ind.target_h;
                invalidate_window_frame(win);
                add_dirty_rect(win->x + ind.target_x - 4, win->y + ind.target_y - 4, ind.target_w + 8, ind.target_h + 8);
            }
            break;
        }

        case WIN_MSG_A11Y_GET_TREE: {
            if (payload_len < sizeof(struct win_msg_a11y_get_tree)) return;
            struct win_msg_a11y_get_tree req;
            memcpy(&req, payload, sizeof(req));

            struct server_window* target_win = find_window_by_id(hdr->window_id);
            if (!target_win) {
                struct win_msg_a11y_result res;
                memset(&res, 0, sizeof(res));
                res.ok = 0;
                snprintf(res.error, sizeof(res.error), "Target window not found");
                send_client_msg(c->fd, WIN_MSG_A11Y_RESULT, hdr->window_id, &res, sizeof(res));
                break;
            }

            uint32_t caller_agent = c->is_privileged ? req.caller_agent_id : c->agent_id;

            /* Check CAP_WINDOW_OBSERVE against target window app allowlist */
            if (caller_agent > 0) {
                if (!check_agent_window_cap(caller_agent, CAP_WINDOW_OBSERVE, target_win->app_name)) {
                    struct win_msg_a11y_result res;
                    memset(&res, 0, sizeof(res));
                    res.ok = 0;
                    snprintf(res.error, sizeof(res.error), "Permission denied: lacks CAP_WINDOW_OBSERVE for %s", target_win->app_name);
                    send_client_msg(c->fd, WIN_MSG_A11Y_RESULT, hdr->window_id, &res, sizeof(res));
                    break;
                }
            }

            target_win->a11y_requester_client_idx = c_idx;
            send_client_msg(clients[target_win->client_idx].fd, WIN_MSG_A11Y_GET_TREE, target_win->id, &req, sizeof(req));
            break;
        }

        case WIN_MSG_A11Y_TREE: {
            struct server_window* win = find_window_by_id(hdr->window_id);
            if (win && win->a11y_requester_client_idx >= 0 && win->a11y_requester_client_idx < MAX_CLIENTS && clients[win->a11y_requester_client_idx].active) {
                send_client_msg(clients[win->a11y_requester_client_idx].fd, WIN_MSG_A11Y_TREE, hdr->window_id, payload, payload_len);
                win->a11y_requester_client_idx = -1;
            }
            break;
        }

        case WIN_MSG_A11Y_ACTION: {
            if (payload_len < sizeof(struct win_msg_a11y_action)) return;
            struct win_msg_a11y_action act;
            memcpy(&act, payload, sizeof(act));
            act.action[sizeof(act.action) - 1] = '\0';
            act.args[sizeof(act.args) - 1] = '\0';

            struct server_window* target_win = find_window_by_id(hdr->window_id);
            if (!target_win) {
                struct win_msg_a11y_result res;
                memset(&res, 0, sizeof(res));
                res.ok = 0;
                snprintf(res.error, sizeof(res.error), "Target window not found");
                send_client_msg(c->fd, WIN_MSG_A11Y_RESULT, hdr->window_id, &res, sizeof(res));
                break;
            }

            uint32_t caller_agent = c->is_privileged ? act.caller_agent_id : c->agent_id;

            /* Capability check: CAP_WINDOW_ACT */
            if (caller_agent > 0) {
                if (!check_agent_window_cap(caller_agent, CAP_WINDOW_ACT, target_win->app_name)) {
                    struct win_msg_a11y_result res;
                    memset(&res, 0, sizeof(res));
                    res.ok = 0;
                    snprintf(res.error, sizeof(res.error), "Permission denied: lacks CAP_WINDOW_ACT for %s", target_win->app_name);
                    send_client_msg(c->fd, WIN_MSG_A11Y_RESULT, hdr->window_id, &res, sizeof(res));
                    break;
                }
            }

            /* Human input preemption: pause action if hardware input within 500ms */
            uint64_t now = get_now_ms();
            if (now >= target_win->last_hardware_input_time && (now - target_win->last_hardware_input_time) < 500) {
                int slot = -1;
                for (int p = 0; p < MAX_PENDING_A11Y_ACTIONS; p++) {
                    if (!g_pending_a11y[p].active) { slot = p; break; }
                }
                if (slot >= 0) {
                    g_pending_a11y[slot].active = 1;
                    g_pending_a11y[slot].resume_time = target_win->last_hardware_input_time + 500;
                    g_pending_a11y[slot].requester_client_idx = c_idx;
                    g_pending_a11y[slot].window_id = target_win->id;
                    g_pending_a11y[slot].caller_agent_id = caller_agent;
                    strncpy(g_pending_a11y[slot].expected_app_name, target_win->app_name, sizeof(g_pending_a11y[slot].expected_app_name) - 1);
                    memcpy(&g_pending_a11y[slot].act, &act, sizeof(act));
                    break;
                } else {
                    struct win_msg_a11y_result res;
                    memset(&res, 0, sizeof(res));
                    res.ok = 0;
                    snprintf(res.error, sizeof(res.error), "Aborted: human input preemption queue full");
                    send_client_msg(c->fd, WIN_MSG_A11Y_RESULT, hdr->window_id, &res, sizeof(res));
                    break;
                }
            }

            /* Visible control indicators */
            target_win->agent_control_id = caller_agent;
            target_win->agent_control_expire_ms = now + 1500;
            invalidate_window_frame(target_win);

            /* Audit log entry with hash of args */
            uint8_t ahash[32];
            wm_sha256_ctx_t hctx;
            wm_sha256_init(&hctx);
            wm_sha256_update(&hctx, (const uint8_t*)act.args, strlen(act.args));
            wm_sha256_final(&hctx, ahash);
            char hex_hash[33];
            wm_hash_to_hex(ahash, hex_hash, sizeof(hex_hash));

            char audit_detail[96];
            snprintf(audit_detail, sizeof(audit_detail), "agent:%u win:%s act:%s hash:%.16s",
                     (unsigned int)caller_agent, target_win->app_name, act.action, hex_hash);
            audit_append((int)caller_agent, AUDIT_EVENT_WINDOW_ACTION, audit_detail);

            target_win->a11y_requester_client_idx = c_idx;
            send_client_msg(clients[target_win->client_idx].fd, WIN_MSG_A11Y_ACTION, target_win->id, &act, sizeof(act));
            break;
        }

        case WIN_MSG_A11Y_RESULT: {
            struct server_window* win = find_window_by_id(hdr->window_id);
            if (win && win->a11y_requester_client_idx >= 0 && win->a11y_requester_client_idx < MAX_CLIENTS && clients[win->a11y_requester_client_idx].active) {
                send_client_msg(clients[win->a11y_requester_client_idx].fd, WIN_MSG_A11Y_RESULT, hdr->window_id, payload, payload_len);
                win->a11y_requester_client_idx = -1;
            }
            break;
        }

        default:
            break;
    }
}

static void read_client_data(int c_idx) {
    struct client* c = &clients[c_idx];
    size_t space = sizeof(c->rx_buf) - c->rx_len;
    if (space == 0) {
        disconnect_client(c_idx, "inbound queue overflow");
        return;
    }

    int nr = read(c->fd, c->rx_buf + c->rx_len, space);
    if (nr <= 0) {
        if (nr < 0 && (errno == EINTR || errno == EAGAIN)) return;
        disconnect_client(c_idx, "client closed connection");
        return;
    }
    c->rx_len += nr;

    while (c->rx_len >= sizeof(struct win_msg_header)) {
        struct win_msg_header hdr;
        memcpy(&hdr, c->rx_buf, sizeof(hdr));

        if (hdr.len < sizeof(hdr) || hdr.len > WIN_MAX_MSG_LEN) {
            disconnect_client(c_idx, "invalid framing header length");
            return;
        }

        if (c->rx_len < hdr.len) {
            break;
        }

        const uint8_t* payload = c->rx_buf + sizeof(hdr);
        uint32_t payload_len = hdr.len - sizeof(hdr);

        handle_client_message(c_idx, &hdr, payload, payload_len);
        if (!c->active) return;

        size_t remaining = c->rx_len - hdr.len;
        if (remaining > 0) {
            memmove(c->rx_buf, c->rx_buf + hdr.len, remaining);
        }
        c->rx_len = remaining;
    }
}

/* Control Socket Handling (wmctl) */
static void handle_ctrl_command(int c_idx, const char* cmd) {
    struct ctrl_client* c = &ctrl_clients[c_idx];
    char resp[2048];
    resp[0] = '\0';

    if (strncmp(cmd, "LIST", 4) == 0) {
        size_t off = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (windows[i].alive) {
                const char* st_str = "normal";
                if (windows[i].state == WIN_STATE_MAXIMIZED) st_str = "maximized";
                else if (windows[i].state == WIN_STATE_MINIMIZED) st_str = "minimized";
                int foc = (windows[i].id == focused_window_id);
                off += snprintf(resp + off, sizeof(resp) - off,
                                "ID %u PID %d TITLE \"%s\" APP \"%s\" AGENT %u X %d Y %d W %u H %u STATE %s FOCUS %d\n",
                                (unsigned int)windows[i].id,
                                (int)clients[windows[i].client_idx].pid,
                                windows[i].title,
                                windows[i].app_name[0] ? windows[i].app_name : "app",
                                (unsigned int)windows[i].agent_id,
                                (int)windows[i].x, (int)windows[i].y,
                                (unsigned int)windows[i].width, (unsigned int)windows[i].height,
                                st_str, foc);
            }
        }
        snprintf(resp + off, sizeof(resp) - off, "OK\n");
    } else if (strncmp(cmd, "FOCUS ", 6) == 0) {
        uint32_t wid = (uint32_t) atoi(cmd + 6);
        struct server_window* win = find_window_by_id(wid);
        if (win) {
            if (win->state == WIN_STATE_MINIMIZED) win->state = WIN_STATE_NORMAL;
            raise_window(wid);
            set_focused_window(wid);
            snprintf(resp, sizeof(resp), "OK\n");
        } else {
            snprintf(resp, sizeof(resp), "ERROR window not found\n");
        }
    } else if (strncmp(cmd, "CLOSE ", 6) == 0) {
        uint32_t wid = (uint32_t) atoi(cmd + 6);
        struct server_window* win = find_window_by_id(wid);
        if (win) {
            send_client_msg(clients[win->client_idx].fd, WIN_MSG_CLOSE_REQUEST, win->id, NULL, 0);
            snprintf(resp, sizeof(resp), "OK\n");
        } else {
            snprintf(resp, sizeof(resp), "ERROR window not found\n");
        }
    } else if (strncmp(cmd, "MOVE ", 5) == 0) {
        unsigned int wid_u = 0;
        int nx = 0, ny = 0;
        if (sscanf(cmd + 5, "%u %d %d", &wid_u, &nx, &ny) == 3) {
            uint32_t wid = (uint32_t) wid_u;
            struct server_window* win = find_window_by_id(wid);
            if (win) {
                invalidate_window_frame(win);
                win->x = nx;
                win->y = ny;
                invalidate_window_frame(win);
                snprintf(resp, sizeof(resp), "OK\n");
            } else {
                snprintf(resp, sizeof(resp), "ERROR window not found\n");
            }
        } else {
            snprintf(resp, sizeof(resp), "ERROR invalid args\n");
        }
    } else if (strncmp(cmd, "MINIMIZE ", 9) == 0) {
        uint32_t wid = (uint32_t) atoi(cmd + 9);
        struct server_window* win = find_window_by_id(wid);
        if (win) {
            invalidate_window_frame(win);
            win->state = WIN_STATE_MINIMIZED;
            if (focused_window_id == win->id) {
                uint32_t next_foc = 0;
                for (int i = num_z_windows - 1; i >= 0; i--) {
                    struct server_window* cand = find_window_by_id(z_order[i]);
                    if (cand && cand->id != win->id && cand->state != WIN_STATE_MINIMIZED) {
                        next_foc = cand->id;
                        break;
                    }
                }
                set_focused_window(next_foc);
            }
            snprintf(resp, sizeof(resp), "OK\n");
        } else {
            snprintf(resp, sizeof(resp), "ERROR window not found\n");
        }
    } else if (strncmp(cmd, "MAXIMIZE ", 9) == 0) {
        uint32_t wid = (uint32_t) atoi(cmd + 9);
        struct server_window* win = find_window_by_id(wid);
        if (win) {
            toggle_maximize(win);
            snprintf(resp, sizeof(resp), "OK\n");
        } else {
            snprintf(resp, sizeof(resp), "ERROR window not found\n");
        }
    } else {
        snprintf(resp, sizeof(resp), "ERROR unknown command\n");
    }

    write(c->fd, resp, strlen(resp));
}

static void read_ctrl_data(int c_idx) {
    struct ctrl_client* c = &ctrl_clients[c_idx];
    size_t space = sizeof(c->rx_buf) - c->rx_len - 1;
    if (space == 0) {
        close(c->fd);
        c->active = 0;
        c->rx_len = 0;
        return;
    }

    int nr = read(c->fd, c->rx_buf + c->rx_len, space);
    if (nr <= 0) {
        if (nr < 0 && (errno == EINTR || errno == EAGAIN)) return;
        close(c->fd);
        c->active = 0;
        c->rx_len = 0;
        return;
    }
    c->rx_len += nr;
    c->rx_buf[c->rx_len] = '\0';

    char* nl = strchr(c->rx_buf, '\n');
    if (nl) {
        *nl = '\0';
        handle_ctrl_command(c_idx, c->rx_buf);
        close(c->fd);
        c->active = 0;
        c->rx_len = 0;
    }
}

/* Hit Testing */
static int hit_test_window(const struct server_window* win, int px, int py) {
    if (!win || !win->alive || win->state == WIN_STATE_MINIMIZED) return HIT_NONE;

    if (win->flags & WIN_FLAG_BORDERLESS) {
        if (px >= win->x && px < win->x + (int)win->width &&
            py >= win->y && py < win->y + (int)win->height) {
            return HIT_CLIENT;
        }
        return HIT_NONE;
    }

    int bw = g_theme.border_width;
    int th = g_theme.titlebar_height;
    int fx, fy, fw, fh;
    get_window_frame(win, &fx, &fy, &fw, &fh);

    if (px < fx || px >= fx + fw || py < fy || py >= fy + fh) {
        return HIT_NONE;
    }

    /* Client Content Area */
    if (px >= win->x && px < win->x + (int)win->width &&
        py >= win->y && py < win->y + (int)win->height) {
        return HIT_CLIENT;
    }

    /* Titlebar Buttons */
    int btn_sz = 16;
    int btn_y = fy + (th - btn_sz) / 2;
    int close_x = fx + fw - bw - 20;
    int max_x   = close_x - 20;
    int min_x   = max_x - 20;

    if (py >= btn_y && py < btn_y + btn_sz) {
        if (px >= close_x && px < close_x + btn_sz) return HIT_CLOSE_BTN;
        if (px >= max_x && px < max_x + btn_sz) return HIT_MAX_BTN;
        if (px >= min_x && px < min_x + btn_sz) return HIT_MIN_BTN;
    }

    /* Corners (10x10) */
    int c_sz = 10;
    if (px < fx + c_sz && py < fy + c_sz) return HIT_CORNER_NW;
    if (px >= fx + fw - c_sz && py < fy + c_sz) return HIT_CORNER_NE;
    if (px < fx + c_sz && py >= fy + fh - c_sz) return HIT_CORNER_SW;
    if (px >= fx + fw - c_sz && py >= fy + fh - c_sz) return HIT_CORNER_SE;

    /* Borders */
    if (py < fy + bw) return HIT_BORDER_N;
    if (py >= fy + fh - bw) return HIT_BORDER_S;
    if (px < fx + bw) return HIT_BORDER_W;
    if (px >= fx + fw - bw) return HIT_BORDER_E;

    /* Remaining Titlebar */
    if (py < fy + th) return HIT_TITLEBAR;

    return HIT_CLIENT;
}

/* Mouse Routing & Interactions */
static void route_mouse_event(int x, int y, int dx, int dy, int btn, int btn_state, int buttons, int wheel) {
    (void) dx; (void) dy;

    if (g_approval.active) {
        if (btn == 1 && btn_state == 1) {
            int dw = 460; int dh = 240;
            int dx = (screen_w - dw) / 2;
            int dy = (screen_h - dh) / 2;
            int by = dy + 180;
            int bh = 34;
            if (y >= by && y < by + bh) {
                if (x >= dx + 20 && x < dx + 130) {
                    wm_resolve_approval(APPROVAL_STATUS_DENY);
                    return;
                } else if (x >= dx + 150 && x < dx + 280) {
                    wm_resolve_approval(APPROVAL_STATUS_ALLOW);
                    return;
                } else if (x >= dx + 300 && x < dx + 440) {
                    wm_resolve_approval(APPROVAL_STATUS_SESSION);
                    return;
                }
            }
        }
        return;
    }

    /* 1. Handle Active Dragging / Resizing */
    if (g_drag_state != DRAG_NONE) {
        struct server_window* dwin = find_window_by_id(g_drag_win_id);
        if (!dwin || !dwin->alive) {
            g_drag_state = DRAG_NONE;
            return;
        }

        if (btn == 1 && btn_state == 0) {
            /* Release drag */
            g_drag_state = DRAG_NONE;
            return;
        }

        if (g_drag_state == DRAG_MOVE) {
            int bw = g_theme.border_width;
            int th = g_theme.titlebar_height;
            int nx = x - g_drag_offset_x;
            int ny = y - g_drag_offset_y;

            /* Clamp to screen: titlebar always accessible */
            if (ny < th) ny = th;
            if (ny > screen_h - 10) ny = screen_h - 10;
            if (nx < bw - (int)dwin->width + 40) nx = bw - (int)dwin->width + 40;
            if (nx > screen_w - bw - 40) nx = screen_w - bw - 40;

            if (nx != dwin->x || ny != dwin->y) {
                invalidate_window_frame(dwin);
                dwin->x = nx;
                dwin->y = ny;
                invalidate_window_frame(dwin);
            }
            return;
        }

        if (g_drag_state == DRAG_RESIZE) {
            int mx_delta = x - g_drag_start_mx;
            int my_delta = y - g_drag_start_my;
            int nw = g_drag_start_w;
            int nh = g_drag_start_h;
            int nx = g_drag_start_x;
            int ny = g_drag_start_y;

            switch (g_drag_hit_zone) {
                case HIT_BORDER_E:
                case HIT_CORNER_NE:
                case HIT_CORNER_SE:
                    nw = g_drag_start_w + mx_delta;
                    break;
                case HIT_BORDER_W:
                case HIT_CORNER_NW:
                case HIT_CORNER_SW:
                    nw = g_drag_start_w - mx_delta;
                    nx = g_drag_start_x + mx_delta;
                    break;
                default: break;
            }

            switch (g_drag_hit_zone) {
                case HIT_BORDER_S:
                case HIT_CORNER_SW:
                case HIT_CORNER_SE:
                    nh = g_drag_start_h + my_delta;
                    break;
                case HIT_BORDER_N:
                case HIT_CORNER_NW:
                case HIT_CORNER_NE:
                    nh = g_drag_start_h - my_delta;
                    ny = g_drag_start_y + my_delta;
                    break;
                default: break;
            }

            if (nw < (int)dwin->min_w) {
                if (nx != g_drag_start_x) nx = g_drag_start_x + (g_drag_start_w - dwin->min_w);
                nw = dwin->min_w;
            }
            if (nw > (int)dwin->max_w) nw = dwin->max_w;
            if (nh < (int)dwin->min_h) {
                if (ny != g_drag_start_y) ny = g_drag_start_y + (g_drag_start_h - dwin->min_h);
                nh = dwin->min_h;
            }
            if (nh > (int)dwin->max_h) nh = dwin->max_h;

            if (nw != (int)dwin->width || nh != (int)dwin->height || nx != dwin->x || ny != dwin->y) {
                invalidate_window_frame(dwin);
                dwin->width = nw;
                dwin->height = nh;
                dwin->x = nx;
                dwin->y = ny;
                invalidate_window_frame(dwin);

                struct win_msg_configure cfg;
                cfg.width = nw;
                cfg.height = nh;
                cfg.state = dwin->state;
                send_client_msg(clients[dwin->client_idx].fd, WIN_MSG_CONFIGURE, dwin->id, &cfg, sizeof(cfg));
            }
            return;
        }
    }

    /* 2. Hit-testing top-to-bottom of Z-order */
    int hit_win_idx = -1;
    int hit_zone = HIT_NONE;

    for (int i = num_z_windows - 1; i >= 0; i--) {
        uint32_t wid = z_order[i];
        for (int w = 0; w < MAX_WINDOWS; w++) {
            if (windows[w].alive && windows[w].id == wid && windows[w].state != WIN_STATE_MINIMIZED) {
                int z = hit_test_window(&windows[w], x, y);
                if (z != HIT_NONE) {
                    hit_win_idx = w;
                    hit_zone = z;
                    break;
                }
            }
        }
        if (hit_win_idx >= 0) break;
    }

    /* 3. Update Cursor Shape & Button Hover */
    int cursor = CURSOR_ARROW;
    if (hit_win_idx >= 0) {
        struct server_window* hwin = &windows[hit_win_idx];
        int new_hover = 0;
        if (hit_zone == HIT_CLOSE_BTN) new_hover = 1;
        else if (hit_zone == HIT_MAX_BTN) new_hover = 2;
        else if (hit_zone == HIT_MIN_BTN) new_hover = 3;

        if (hwin->button_hover != new_hover) {
            hwin->button_hover = new_hover;
            invalidate_window_frame(hwin);
        }

        switch (hit_zone) {
            case HIT_BORDER_W:
            case HIT_BORDER_E:
                cursor = CURSOR_RESIZE_EW;
                break;
            case HIT_BORDER_N:
            case HIT_BORDER_S:
                cursor = CURSOR_RESIZE_NS;
                break;
            case HIT_CORNER_NW:
            case HIT_CORNER_SE:
                cursor = CURSOR_RESIZE_NWSE;
                break;
            case HIT_CORNER_NE:
            case HIT_CORNER_SW:
                cursor = CURSOR_RESIZE_NESW;
                break;
            default:
                cursor = CURSOR_ARROW;
                break;
        }
    }
    g_current_cursor = cursor;

    /* 4. Pointer enter / leave client dispatch */
    int client_hover_idx = (hit_zone == HIT_CLIENT) ? hit_win_idx : -1;
    if (client_hover_idx != hovered_window_idx) {
        if (hovered_window_idx >= 0 && windows[hovered_window_idx].alive) {
            send_client_msg(clients[windows[hovered_window_idx].client_idx].fd,
                            WIN_MSG_POINTER_LEAVE, windows[hovered_window_idx].id, NULL, 0);
        }
        hovered_window_idx = client_hover_idx;
        if (hovered_window_idx >= 0 && windows[hovered_window_idx].alive) {
            struct win_msg_pointer_enter pe;
            pe.x = x - windows[hovered_window_idx].x;
            pe.y = y - windows[hovered_window_idx].y;
            send_client_msg(clients[windows[hovered_window_idx].client_idx].fd,
                            WIN_MSG_POINTER_ENTER, windows[hovered_window_idx].id, &pe, sizeof(pe));
        }
    }

    /* 5. Left Click Down Actions */
    if (btn == 1 && btn_state == 1) {
        if (hit_win_idx >= 0) {
            struct server_window* twin = &windows[hit_win_idx];
            raise_window(twin->id);
            set_focused_window(twin->id);

            if (hit_zone == HIT_CLOSE_BTN) {
                send_client_msg(clients[twin->client_idx].fd, WIN_MSG_CLOSE_REQUEST, twin->id, NULL, 0);
                return;
            } else if (hit_zone == HIT_MAX_BTN) {
                toggle_maximize(twin);
                return;
            } else if (hit_zone == HIT_MIN_BTN) {
                invalidate_window_frame(twin);
                twin->state = WIN_STATE_MINIMIZED;
                uint32_t next_foc = 0;
                for (int i = num_z_windows - 1; i >= 0; i--) {
                    struct server_window* cand = find_window_by_id(z_order[i]);
                    if (cand && cand->id != twin->id && cand->state != WIN_STATE_MINIMIZED) {
                        next_foc = cand->id;
                        break;
                    }
                }
                set_focused_window(next_foc);
                return;
            } else if (hit_zone == HIT_TITLEBAR) {
                if (twin->state != WIN_STATE_MAXIMIZED) {
                    g_drag_state = DRAG_MOVE;
                    g_drag_win_id = twin->id;
                    g_drag_offset_x = x - twin->x;
                    g_drag_offset_y = y - twin->y;
                }
                return;
            } else if (hit_zone >= HIT_BORDER_W && hit_zone <= HIT_CORNER_SE) {
                if (twin->state != WIN_STATE_MAXIMIZED) {
                    g_drag_state = DRAG_RESIZE;
                    g_drag_win_id = twin->id;
                    g_drag_hit_zone = hit_zone;
                    g_drag_start_x = twin->x;
                    g_drag_start_y = twin->y;
                    g_drag_start_w = twin->width;
                    g_drag_start_h = twin->height;
                    g_drag_start_mx = x;
                    g_drag_start_my = y;
                }
                return;
            }
        } else {
            set_focused_window(0);
        }
    }

    /* 6. Client Event Forwarding */
    if (hit_win_idx >= 0 && hit_zone == HIT_CLIENT) {
        struct server_window* cwin = &windows[hit_win_idx];
        if (btn != 0) {
            struct win_msg_pointer_button pb;
            pb.button = btn;
            pb.state = btn_state;
            pb.x = x - cwin->x;
            pb.y = y - cwin->y;
            send_client_msg(clients[cwin->client_idx].fd,
                            WIN_MSG_POINTER_BUTTON, cwin->id, &pb, sizeof(pb));
        } else if (wheel != 0) {
            struct win_msg_pointer_wheel pw;
            pw.dx = 0;
            pw.dy = wheel;
            send_client_msg(clients[cwin->client_idx].fd,
                            WIN_MSG_POINTER_WHEEL, cwin->id, &pw, sizeof(pw));
        } else {
            struct win_msg_pointer_move pm;
            pm.x = x - cwin->x;
            pm.y = y - cwin->y;
            send_client_msg(clients[cwin->client_idx].fd,
                            WIN_MSG_POINTER_MOVE, cwin->id, &pm, sizeof(pm));
        }
    }
}

/* Keyboard Event Routing & Global Shortcuts */
static void route_keyboard_event(uint32_t keycode, uint32_t action, uint32_t mods) {
    if (g_approval.active) {
        if (action == WIN_KEY_PRESS) {
            if (keycode == KEY_ESC) {
                wm_resolve_approval(APPROVAL_STATUS_DENY);
            } else if (keycode == 15 || keycode == 106) { /* Tab or Right Arrow */
                g_approval.focused_btn = (g_approval.focused_btn + 1) % 3;
                add_dirty_rect(0, 0, screen_w, screen_h);
            } else if (keycode == 105) { /* Left Arrow */
                g_approval.focused_btn = (g_approval.focused_btn + 2) % 3;
                add_dirty_rect(0, 0, screen_w, screen_h);
            } else if (keycode == 28 || keycode == 57) { /* Enter or Space */
                if (g_approval.focused_btn == 0) wm_resolve_approval(APPROVAL_STATUS_DENY);
                else if (g_approval.focused_btn == 1) wm_resolve_approval(APPROVAL_STATUS_ALLOW);
                else if (g_approval.focused_btn == 2) wm_resolve_approval(APPROVAL_STATUS_SESSION);
            }
        }
        return;
    }

    g_last_mods = mods;
    int alt_down = (mods & (KBD_MOD_LALT | KBD_MOD_RALT)) != 0;
    static int s_super_down = 0;
    if (keycode == 125 || keycode == 126) {
        s_super_down = (action == WIN_KEY_PRESS);
    }

    /* Global Shortcut: Super+Space (Agent Panel) */
    if (s_super_down && keycode == 57 && action == WIN_KEY_PRESS) {
        int found = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (windows[i].alive && strcmp(windows[i].app_name, "agentpanel") == 0) {
                set_focused_window(windows[i].id);
                raise_window(windows[i].id);
                found = 1;
                break;
            }
        }
        if (!found) {
            pid_t p = fork();
            if (p == 0) {
                char* argv[] = { "/bin/agentpanel.elf", NULL };
                execv("/bin/agentpanel.elf", argv);
                execv("agentpanel.elf", argv);
                _exit(1);
            }
        }
        return;
    }

    /* Global Shortcut: Alt+Tab (Window Switcher) */
    if (alt_down && keycode == KEY_TAB && action == WIN_KEY_PRESS) {
        if (!g_switcher_active) {
            g_switcher_count = 0;
            for (int i = num_z_windows - 1; i >= 0; i--) {
                struct server_window* w = find_window_by_id(z_order[i]);
                if (w && w->alive) {
                    g_switcher_list[g_switcher_count++] = w->id;
                }
            }
            if (g_switcher_count > 0) {
                g_switcher_active = 1;
                g_switcher_idx = (g_switcher_count > 1) ? 1 : 0;
                add_dirty_rect((screen_w - 460) / 2, (screen_h - 100) / 2, 460, 100);
            }
        } else {
            if (g_switcher_count > 0) {
                g_switcher_idx = (g_switcher_idx + 1) % g_switcher_count;
                add_dirty_rect((screen_w - 460) / 2, (screen_h - 100) / 2, 460, 100);
            }
        }
        return;
    }

    /* Alt Key Release: Dismiss Switcher & Activate Selection */
    if (g_switcher_active && (keycode == KEY_LEFTALT || keycode == KEY_RIGHTALT) && action == WIN_KEY_RELEASE) {
        g_switcher_active = 0;
        add_dirty_rect((screen_w - 460) / 2, (screen_h - 100) / 2, 460, 100);
        if (g_switcher_count > 0 && g_switcher_idx < g_switcher_count) {
            uint32_t chosen_id = g_switcher_list[g_switcher_idx];
            struct server_window* win = find_window_by_id(chosen_id);
            if (win) {
                if (win->state == WIN_STATE_MINIMIZED) win->state = WIN_STATE_NORMAL;
                raise_window(chosen_id);
                set_focused_window(chosen_id);
            }
        }
        return;
    }

    /* Global Shortcut: Alt+F4 (Close Focused Window) */
    if (alt_down && keycode == KEY_F4 && action == WIN_KEY_PRESS) {
        if (focused_window_id != 0) {
            struct server_window* fw = find_window_by_id(focused_window_id);
            if (fw) {
                send_client_msg(clients[fw->client_idx].fd, WIN_MSG_CLOSE_REQUEST, fw->id, NULL, 0);
            }
        }
        return;
    }

    /* Global Shortcut: Super / Win or Ctrl+Esc (Start Menu Stub) */
    int ctrl_down = (mods & (KBD_MOD_LCTRL | KBD_MOD_RCTRL)) != 0;
    if (((keycode == 125 || keycode == 126) || (ctrl_down && keycode == KEY_ESC)) && action == WIN_KEY_PRESS) {
        printf("[wm] Super / Start Menu toggle shortcut triggered\n");
        return;
    }

    /* Forward to focused window */
    if (focused_window_id == 0) return;
    struct server_window* win = find_window_by_id(focused_window_id);
    if (!win || !win->alive) return;

    uint32_t unicode = translate_key(keycode, mods);

    struct win_msg_key k;
    k.keycode = keycode;
    k.unicode = unicode;
    k.modifiers = mods;
    k.action = action;

    send_client_msg(clients[win->client_idx].fd, WIN_MSG_KEY, win->id, &k, sizeof(k));
}

/* Drop Shadow Rendering (4px semi-transparent black) */
static void draw_drop_shadow(gfx_surface_t* s, int fx, int fy, int fw, int fh, int offset, const struct rect* dr) {
    int sx0 = fx + offset;
    int sy0 = fy + offset;
    int sx1 = sx0 + fw;
    int sy1 = sy0 + fh;

    int x0 = (sx0 > dr->x) ? sx0 : dr->x;
    int y0 = (sy0 > dr->y) ? sy0 : dr->y;
    int x1 = (sx1 < dr->x + dr->w) ? sx1 : (dr->x + dr->w);
    int y1 = (sy1 < dr->y + dr->h) ? sy1 : (dr->y + dr->h);

    if (x1 <= x0 || y1 <= y0) return;

    int pitch_words = s->pitch / sizeof(uint32_t);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) continue;
            uint32_t orig = s->pixels[y * pitch_words + x];
            uint32_t r = ((orig >> 16) & 0xFF) / 2;
            uint32_t g = ((orig >> 8) & 0xFF) / 2;
            uint32_t b = (orig & 0xFF) / 2;
            s->pixels[y * pitch_words + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

/* Server-Side Decorations Drawing */
static void draw_window_decorations(gfx_surface_t* s, struct server_window* win) {
    if (win->flags & WIN_FLAG_BORDERLESS) return;

    int bw = g_theme.border_width;
    int th = g_theme.titlebar_height;
    int fx, fy, fw, fh;
    get_window_frame(win, &fx, &fy, &fw, &fh);

    int is_active = (win->id == focused_window_id);
    uint32_t border_col = is_active ? g_theme.active_border : g_theme.inactive_border;
    uint32_t tb_col     = is_active ? g_theme.active_titlebar : g_theme.inactive_titlebar;

    if (win->agent_control_id > 0 && get_now_ms() < win->agent_control_expire_ms) {
        border_col = s_agent_colors[(win->agent_control_id - 1) % 6];
    }

    /* 1. Outer Border */
    gfx_fill_rect(s, fx, fy, fw, bw, border_col);
    gfx_fill_rect(s, fx, fy + fh - bw, fw, bw, border_col);
    gfx_fill_rect(s, fx, fy + bw, bw, fh - 2 * bw, border_col);
    gfx_fill_rect(s, fx + fw - bw, fy + bw, bw, fh - 2 * bw, border_col);

    /* 2. Titlebar */
    int tbx = fx + bw;
    int tby = fy + bw;
    int tbw = fw - 2 * bw;
    int tbh = th - bw;
    gfx_fill_rect(s, tbx, tby, tbw, tbh, tb_col);

    /* 3. Buttons */
    int btn_sz = 16;
    int btn_y = fy + (th - btn_sz) / 2;

    /* Close Button */
    int close_x = fx + fw - bw - 20;
    uint32_t close_bg = (win->button_hover == 1) ? 0xFFEF4444 : (is_active ? 0xFF1E293B : 0xFF334155);
    gfx_fill_rect(s, close_x, btn_y, btn_sz, btn_sz, close_bg);
    gfx_draw_line(s, close_x + 4, btn_y + 4, close_x + 11, btn_y + 11, 0xFFFFFFFF);
    gfx_draw_line(s, close_x + 5, btn_y + 4, close_x + 12, btn_y + 11, 0xFFFFFFFF);
    gfx_draw_line(s, close_x + 11, btn_y + 4, close_x + 4, btn_y + 11, 0xFFFFFFFF);
    gfx_draw_line(s, close_x + 12, btn_y + 4, close_x + 5, btn_y + 11, 0xFFFFFFFF);

    /* Maximize/Restore Button */
    int max_x = close_x - 20;
    uint32_t max_bg = (win->button_hover == 2) ? 0xFF475569 : (is_active ? 0xFF1E293B : 0xFF334155);
    gfx_fill_rect(s, max_x, btn_y, btn_sz, btn_sz, max_bg);
    if (win->state == WIN_STATE_MAXIMIZED) {
        gfx_draw_rect(s, max_x + 3, btn_y + 5, 8, 8, 0xFFFFFFFF);
        gfx_draw_line(s, max_x + 5, btn_y + 3, max_x + 12, btn_y + 3, 0xFFFFFFFF);
        gfx_draw_line(s, max_x + 12, btn_y + 3, max_x + 12, btn_y + 10, 0xFFFFFFFF);
    } else {
        gfx_draw_rect(s, max_x + 3, btn_y + 3, 10, 10, 0xFFFFFFFF);
        gfx_draw_line(s, max_x + 3, btn_y + 4, max_x + 12, btn_y + 4, 0xFFFFFFFF);
    }

    /* Minimize Button */
    int min_x = max_x - 20;
    uint32_t min_bg = (win->button_hover == 3) ? 0xFF475569 : (is_active ? 0xFF1E293B : 0xFF334155);
    gfx_fill_rect(s, min_x, btn_y, btn_sz, btn_sz, min_bg);
    gfx_draw_line(s, min_x + 3, btn_y + 11, min_x + 12, btn_y + 11, 0xFFFFFFFF);
    gfx_draw_line(s, min_x + 3, btn_y + 12, min_x + 12, btn_y + 12, 0xFFFFFFFF);

    /* 4. Title Text (truncated with "..." if necessary) */
    int text_x = fx + bw + 8;
    int text_y = fy + (th - 16) / 2;
    int max_text_w = min_x - 6 - text_x;
    if (max_text_w > 16) {
        char title_buf[128];
        int max_chars = max_text_w / 8;
        int title_len = (int)strlen(win->title);
        if (title_len > max_chars && max_chars > 3) {
            strncpy(title_buf, win->title, max_chars - 3);
            title_buf[max_chars - 3] = '\0';
            strcat(title_buf, "...");
        } else {
            strncpy(title_buf, win->title, sizeof(title_buf) - 1);
            title_buf[sizeof(title_buf) - 1] = '\0';
        }

        if (win->agent_control_id > 0 && get_now_ms() < win->agent_control_expire_ms) {
            char badge[48];
            snprintf(badge, sizeof(badge), " [Agent: %s]", win->agent_control_name[0] ? win->agent_control_name : "active");
            strncat(title_buf, badge, sizeof(title_buf) - strlen(title_buf) - 1);
        }

        gfx_draw_text_transparent(s, text_x, text_y, title_buf, g_theme.titlebar_text);
    }
}

/* Window Switcher Overlay */
static void draw_window_switcher(gfx_surface_t* s) {
    if (!g_switcher_active || g_switcher_count == 0) return;

    int box_w = 460;
    int box_h = 100;
    int box_x = (screen_w - box_w) / 2;
    int box_y = (screen_h - box_h) / 2;

    gfx_fill_rect(s, box_x, box_y, box_w, box_h, 0xFF1E293B);
    gfx_draw_rect(s, box_x, box_y, box_w, box_h, 0xFF38BDF8);
    gfx_draw_rect(s, box_x + 1, box_y + 1, box_w - 2, box_h - 2, 0xFF0284C7);

    gfx_draw_text_transparent(s, box_x + 14, box_y + 10, "Window Switcher (Alt+Tab)", 0xFF38BDF8);

    int card_w = 80;
    int card_h = 50;
    int card_spacing = 10;
    int total_cards_w = g_switcher_count * card_w + (g_switcher_count - 1) * card_spacing;
    int start_x = box_x + (box_w - total_cards_w) / 2;
    if (start_x < box_x + 10) start_x = box_x + 10;
    int start_y = box_y + 35;

    for (int i = 0; i < g_switcher_count && i < 5; i++) {
        uint32_t wid = g_switcher_list[i];
        struct server_window* win = find_window_by_id(wid);
        if (!win) continue;

        int cx = start_x + i * (card_w + card_spacing);
        int is_selected = (i == g_switcher_idx);

        uint32_t card_bg = is_selected ? 0xFF2563EB : 0xFF334155;
        uint32_t border_col = is_selected ? 0xFF60A5FA : 0xFF64748B;
        gfx_fill_rect(s, cx, start_y, card_w, card_h, card_bg);
        gfx_draw_rect(s, cx, start_y, card_w, card_h, border_col);

        char title_short[10];
        strncpy(title_short, win->title, 8);
        title_short[8] = '\0';
        gfx_draw_text_transparent(s, cx + 6, start_y + 8, title_short, is_selected ? 0xFFFFFFFF : 0xFFCBD5E1);

        char id_str[16];
        snprintf(id_str, sizeof(id_str), "#%u", (unsigned int)win->id);
        gfx_draw_text_transparent(s, cx + 6, start_y + 28, id_str, is_selected ? 0xFF93C5FD : 0xFF94A3B8);
    }
}

/* Custom Cursor Rendering */
static void draw_custom_cursor(gfx_surface_t* s, int x, int y, int type) {
    const char** shape = s_cursor_arrow;
    int hot_x = 0, hot_y = 0;

    switch (type) {
        case CURSOR_RESIZE_EW:
            shape = s_cursor_ew;
            hot_x = 7; hot_y = 8;
            break;
        case CURSOR_RESIZE_NS:
            shape = s_cursor_ns;
            hot_x = 7; hot_y = 8;
            break;
        case CURSOR_RESIZE_NWSE:
            shape = s_cursor_nwse;
            hot_x = 7; hot_y = 7;
            break;
        case CURSOR_RESIZE_NESW:
            shape = s_cursor_nesw;
            hot_x = 7; hot_y = 7;
            break;
        default:
            shape = s_cursor_arrow;
            hot_x = 0; hot_y = 0;
            break;
    }

    int px0 = x - hot_x;
    int py0 = y - hot_y;

    for (int row = 0; row < 16; row++) {
        int py = py0 + row;
        if (py < s->clip.y0 || py >= s->clip.y1) continue;

        for (int col = 0; col < 16; col++) {
            int px = px0 + col;
            if (px < s->clip.x0 || px >= s->clip.x1) continue;

            char ch = shape[row][col];
            if (ch == 'X') {
                gfx_putpixel(s, px, py, GFX_COLOR_BLACK);
            } else if (ch == '.') {
                gfx_putpixel(s, px, py, GFX_COLOR_WHITE);
            }
        }
    }
}

static void wm_resolve_approval(int status) {
    if (!g_approval.active) return;
    if (g_approval.intentd_fd >= 0) {
        struct approval_msg_header hdr;
        hdr.len = sizeof(hdr) + sizeof(struct approval_msg_resolve);
        hdr.type = APPROVAL_MSG_RESOLVE;
        hdr.flags = 0;
        hdr.call_id = 0;

        struct approval_msg_resolve res;
        res.approval_id = g_approval.approval_id;
        res.status = status;
        write(g_approval.intentd_fd, &hdr, sizeof(hdr));
        write(g_approval.intentd_fd, &res, sizeof(res));
    }
    g_approval.active = 0;
    add_dirty_rect(0, 0, screen_w, screen_h);
}

/* Compositing Engine */
static void composite_screen(void) {
    static int s_last_ks_state = 0;
    int cur_ks = agent_killswitch_state();
    if (cur_ks != s_last_ks_state) {
        printf("[wm] Killswitch changed: cur_ks=%d, was %d\n", cur_ks, s_last_ks_state);
        s_last_ks_state = cur_ks;
        add_dirty_rect(0, 0, screen_w, 24);
    }

    if (num_dirty_rects == 0) return;

    for (int d = 0; d < num_dirty_rects; d++) {
        struct rect dr = dirty_rects[d];

        /* 1. Desktop background (#2A4D69) */
        uint32_t bg_color = 0xFF2A4D69;
        gfx_fill_rect(ctx->backbuffer, dr.x, dr.y, dr.w, dr.h, bg_color);

        /* 2. Composite windows in Z-order */
        for (int z = 0; z < num_z_windows; z++) {
            struct server_window* win = find_window_by_id(z_order[z]);
            if (!win || !win->alive || win->state == WIN_STATE_MINIMIZED) continue;

            int fx, fy, fw, fh;
            get_window_frame(win, &fx, &fy, &fw, &fh);

            /* Drop shadow */
            draw_drop_shadow(ctx->backbuffer, fx, fy, fw, fh, 4, &dr);

            /* Server-side decorations */
            draw_window_decorations(ctx->backbuffer, win);

            /* Client buffer contents */
            if (win->active_buffer < 0) continue;
            struct client_buffer* buf = &win->buffers[win->active_buffer];
            if (!buf->valid || !buf->pixels) continue;

            int ix0 = (win->x > dr.x) ? win->x : dr.x;
            int iy0 = (win->y > dr.y) ? win->y : dr.y;
            int ix1 = ((win->x + (int)win->width) < (dr.x + dr.w)) ? (win->x + (int)win->width) : (dr.x + dr.w);
            int iy1 = ((win->y + (int)win->height) < (dr.y + dr.h)) ? (win->y + (int)win->height) : (dr.y + dr.h);

            if (ix1 <= ix0 || iy1 <= iy0) continue;

            int iw = ix1 - ix0;
            int ih = iy1 - iy0;
            int src_x = ix0 - win->x;
            int src_y = iy0 - win->y;

            struct stat st;
            if (buf->fd < 0 || fstat(buf->fd, &st) < 0 || (size_t)st.st_size < buf->size) {
                disconnect_client(win->client_idx, "shm truncated at composite");
                continue;
            }

            g_faulting_client = win->client_idx;
            g_in_client_blit = 1;
            if (sigsetjmp(g_bus_jmp, 1) != 0) {
                g_in_client_blit = 0;
                disconnect_client(g_faulting_client, "SIGBUS encountered during client pixel read");
                continue;
            }

            int stride_words = buf->stride / sizeof(uint32_t);
            int bb_pitch_words = ctx->backbuffer->pitch / sizeof(uint32_t);

            for (int r = 0; r < ih; r++) {
                const uint32_t* src_row = buf->pixels + (src_y + r) * stride_words + src_x;
                uint32_t* dst_row = ctx->backbuffer->pixels + (iy0 + r) * bb_pitch_words + ix0;
                memcpy(dst_row, src_row, iw * sizeof(uint32_t));
            }

            g_in_client_blit = 0;

            /* Target widget bounding box indicator */
            if (win->target_indicator_active && win->agent_control_id > 0 && get_now_ms() < win->agent_control_expire_ms) {
                int hx = win->x + win->target_x;
                int hy = win->y + win->target_y;
                int hw = win->target_w;
                int hh = win->target_h;
                uint32_t col = s_agent_colors[(win->agent_control_id - 1) % 6];
                gfx_draw_rect(ctx->backbuffer, hx, hy, hw, hh, col);
                gfx_draw_rect(ctx->backbuffer, hx + 1, hy + 1, hw - 2, hh - 2, col);
            }
        }

        /* 3. Switcher overlay */
        if (g_switcher_active) {
            draw_window_switcher(ctx->backbuffer);
        }

        /* 3b. Trusted Approval Dialog */
        if (g_approval.active) {
            uint64_t now = get_now_ms();
            if (now >= g_approval.expires_ms) {
                wm_resolve_approval(APPROVAL_STATUS_DENY);
            } else {
                int dw = 460;
                int dh = 240;
                int dx = (screen_w - dw) / 2;
                int dy = (screen_h - dh) / 2;

                /* Modal dialog card */
                gfx_fill_rect(ctx->backbuffer, dx, dy, dw, dh, 0xFF1E293B);
                gfx_draw_rect(ctx->backbuffer, dx, dy, dw, dh, 0xFFEF4444);
                gfx_draw_rect(ctx->backbuffer, dx + 1, dy + 1, dw - 2, dh - 2, 0xFFEF4444);

                /* Title header */
                gfx_fill_rect(ctx->backbuffer, dx, dy, dw, 32, 0xFFDC2626);
                gfx_draw_text_transparent(ctx->backbuffer, dx + 12, dy + 8, "[!] AGENT APPROVAL REQUIRED", 0xFFFFFFFF);

                /* Details */
                char line[128];
                snprintf(line, sizeof(line), "Agent: %s (ID %u)", g_approval.agent_name, (unsigned int)g_approval.agent_id);
                gfx_draw_text_transparent(ctx->backbuffer, dx + 20, dy + 48, line, 0xFFF8FAFC);

                snprintf(line, sizeof(line), "Skill: %s", g_approval.skill);
                gfx_draw_text_transparent(ctx->backbuffer, dx + 20, dy + 72, line, 0xFF38BDF8);

                char sum_line[128];
                snprintf(sum_line, sizeof(sum_line), "Args: %.45s%s", g_approval.args_summary, strlen(g_approval.args_summary) > 45 ? "..." : "");
                gfx_draw_text_transparent(ctx->backbuffer, dx + 20, dy + 96, sum_line, 0xFFCBD5E1);

                int sec_left = (int)((g_approval.expires_ms - now) / 1000);
                snprintf(line, sizeof(line), "Auto-deny in: %ds", sec_left > 0 ? sec_left : 0);
                gfx_draw_text_transparent(ctx->backbuffer, dx + 20, dy + 124, line, 0xFFF59E0B);

                /* Buttons: Deny (0), Allow Once (1), Allow Session (2) */
                int by = dy + 180;
                int bh = 34;

                /* Button 0: Deny */
                int b0_x = dx + 20; int b0_w = 110;
                uint32_t b0_bg = (g_approval.focused_btn == 0) ? 0xFFDC2626 : 0xFF991B1B;
                gfx_fill_rect(ctx->backbuffer, b0_x, by, b0_w, bh, b0_bg);
                gfx_draw_rect(ctx->backbuffer, b0_x, by, b0_w, bh, (g_approval.focused_btn == 0) ? 0xFFFFFFFF : 0xFFEF4444);
                gfx_draw_text_transparent(ctx->backbuffer, b0_x + 30, by + 10, "Deny", 0xFFFFFFFF);

                /* Button 1: Allow Once */
                int b1_x = dx + 150; int b1_w = 130;
                uint32_t b1_bg = (g_approval.focused_btn == 1) ? 0xFF2563EB : 0xFF1E3A8A;
                gfx_fill_rect(ctx->backbuffer, b1_x, by, b1_w, bh, b1_bg);
                gfx_draw_rect(ctx->backbuffer, b1_x, by, b1_w, bh, (g_approval.focused_btn == 1) ? 0xFFFFFFFF : 0xFF3B82F6);
                gfx_draw_text_transparent(ctx->backbuffer, b1_x + 18, by + 10, "Allow Once", 0xFFFFFFFF);

                /* Button 2: Allow Session */
                int b2_x = dx + 300; int b2_w = 140;
                uint32_t b2_bg = (g_approval.focused_btn == 2) ? 0xFF059669 : 0xFF065F46;
                gfx_fill_rect(ctx->backbuffer, b2_x, by, b2_w, bh, b2_bg);
                gfx_draw_rect(ctx->backbuffer, b2_x, by, b2_w, bh, (g_approval.focused_btn == 2) ? 0xFFFFFFFF : 0xFF10B981);
                gfx_draw_text_transparent(ctx->backbuffer, b2_x + 14, by + 10, "Allow Session", 0xFFFFFFFF);
            }
        }

        /* 3c. Killswitch Emergency Banner */
        if (agent_killswitch_state()) {
            gfx_fill_rect(ctx->backbuffer, 0, 0, screen_w, 22, 0xFFEF4444);
            gfx_draw_text_transparent(ctx->backbuffer, (screen_w - 530) / 2, 3,
                          "[EMERGENCY STOP: ALL AGENTS SUSPENDED - Press Ctrl+Alt+Esc to resume]", 0xFFFFFFFF);
        }

        /* 4. Mouse cursor */
        if (mouse_x >= dr.x - 20 && mouse_x < dr.x + dr.w + 20 &&
            mouse_y >= dr.y - 20 && mouse_y < dr.y + dr.h + 20) {
            draw_custom_cursor(ctx->backbuffer, mouse_x, mouse_y, g_current_cursor);
        }

        /* 5. Flush to framebuffer */
        gfx_flip_rect(ctx, dr.x, dr.y, dr.w, dr.h);
    }

    num_dirty_rects = 0;
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;

    printf("[wm] Initializing MyOS Window Manager & Compositor (Stage 18B-2)...\n");
    fflush(stdout);

    /* Signal handlers */
    struct sigaction sa_bus;
    memset(&sa_bus, 0, sizeof(sa_bus));
    sa_bus.sa_handler = sigbus_handler;
    sigaction(SIGBUS, &sa_bus, NULL);

    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Load desktop configuration & keymap */
    load_desktop_config("/etc/myos/desktop.conf");
    load_keymap(g_theme.keymap_path);

    /* Graphics initialization */
    ctx = gfx_open();
    if (!ctx) {
        fprintf(stderr, "[wm] Error: failed to open graphics context (/dev/fb0)\n");
        return 1;
    }

    screen_w = ctx->width;
    screen_h = ctx->height;
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    prev_mouse_x = mouse_x;
    prev_mouse_y = mouse_y;

    /* Display Server Socket (/run/display.sock) */
    unlink(WIN_SOCK_PATH);
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("[wm] display socket");
        gfx_close(ctx);
        return 1;
    }
    fcntl(srv_fd, F_SETFL, fcntl(srv_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WIN_SOCK_PATH, sizeof(sun.sun_path) - 1);
    if (bind(srv_fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        perror("[wm] display bind");
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }
    if (listen(srv_fd, 16) < 0) {
        perror("[wm] display listen");
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }

    /* Control Server Socket (/run/wm.sock) */
    unlink(WM_CTRL_SOCK_PATH);
    int ctrl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_fd < 0) {
        perror("[wm] ctrl socket");
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }
    fcntl(ctrl_fd, F_SETFL, fcntl(ctrl_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_un ctrl_sun;
    memset(&ctrl_sun, 0, sizeof(ctrl_sun));
    ctrl_sun.sun_family = AF_UNIX;
    strncpy(ctrl_sun.sun_path, WM_CTRL_SOCK_PATH, sizeof(ctrl_sun.sun_path) - 1);
    if (bind(ctrl_fd, (struct sockaddr*)&ctrl_sun, sizeof(ctrl_sun)) < 0) {
        perror("[wm] ctrl bind");
        close(ctrl_fd);
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }
    if (listen(ctrl_fd, 8) < 0) {
        perror("[wm] ctrl listen");
        close(ctrl_fd);
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }

    printf("[wm] Display server listening on %s\n", WIN_SOCK_PATH);
    printf("[wm] Control server listening on %s\n", WM_CTRL_SOCK_PATH);
    fflush(stdout);

    /* Approval Server Socket (/run/approval.sock) */
    unlink(APPROVAL_SOCK_PATH);
    int app_srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app_srv_fd >= 0) {
        fcntl(app_srv_fd, F_SETFL, fcntl(app_srv_fd, F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_un app_sun;
        memset(&app_sun, 0, sizeof(app_sun));
        app_sun.sun_family = AF_UNIX;
        strncpy(app_sun.sun_path, APPROVAL_SOCK_PATH, sizeof(app_sun.sun_path) - 1);
        if (bind(app_srv_fd, (struct sockaddr*)&app_sun, sizeof(app_sun)) == 0) {
            listen(app_srv_fd, 4);
            g_approval_srv_fd = app_srv_fd;
            printf("[wm] Approval server listening on %s\n", APPROVAL_SOCK_PATH);
        } else {
            close(app_srv_fd);
        }
    }

    /* Initial screen repaint */
    add_dirty_rect(0, 0, screen_w, screen_h);
    composite_screen();

    /* Main Compositor Loop */
    while (g_running) {
        struct pollfd pfds[MAX_CLIENTS + MAX_CTRL_CLIENTS + 8];
        int num_pfds = 0;

        int mouse_pfd_idx = -1;
        if (ctx->mouse_fd >= 0) {
            mouse_pfd_idx = num_pfds;
            pfds[num_pfds].fd = ctx->mouse_fd;
            pfds[num_pfds].events = POLLIN;
            pfds[num_pfds].revents = 0;
            num_pfds++;
        }

        int kbd_pfd_idx = -1;
        if (ctx->kbd_fd >= 0) {
            kbd_pfd_idx = num_pfds;
            pfds[num_pfds].fd = ctx->kbd_fd;
            pfds[num_pfds].events = POLLIN;
            pfds[num_pfds].revents = 0;
            num_pfds++;
        }

        int srv_pfd_idx = num_pfds;
        pfds[num_pfds].fd = srv_fd;
        pfds[num_pfds].events = POLLIN;
        pfds[num_pfds].revents = 0;
        num_pfds++;

        int ctrl_srv_pfd_idx = num_pfds;
        pfds[num_pfds].fd = ctrl_fd;
        pfds[num_pfds].events = POLLIN;
        pfds[num_pfds].revents = 0;
        num_pfds++;

        int app_srv_pfd_idx = -1;
        if (g_approval_srv_fd >= 0) {
            app_srv_pfd_idx = num_pfds;
            pfds[num_pfds].fd = g_approval_srv_fd;
            pfds[num_pfds].events = POLLIN;
            pfds[num_pfds].revents = 0;
            num_pfds++;
        }

        int app_cl_pfd_idx = -1;
        if (g_approval_cl_fd >= 0) {
            app_cl_pfd_idx = num_pfds;
            pfds[num_pfds].fd = g_approval_cl_fd;
            pfds[num_pfds].events = POLLIN;
            pfds[num_pfds].revents = 0;
            num_pfds++;
        }

        int client_pfd_map[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_pfd_map[i] = -1;
            if (clients[i].active && clients[i].fd >= 0) {
                client_pfd_map[i] = num_pfds;
                pfds[num_pfds].fd = clients[i].fd;
                pfds[num_pfds].events = POLLIN;
                pfds[num_pfds].revents = 0;
                num_pfds++;
            }
        }

        int ctrl_pfd_map[MAX_CTRL_CLIENTS];
        for (int i = 0; i < MAX_CTRL_CLIENTS; i++) {
            ctrl_pfd_map[i] = -1;
            if (ctrl_clients[i].active && ctrl_clients[i].fd >= 0) {
                ctrl_pfd_map[i] = num_pfds;
                pfds[num_pfds].fd = ctrl_clients[i].fd;
                pfds[num_pfds].events = POLLIN;
                pfds[num_pfds].revents = 0;
                num_pfds++;
            }
        }

        int ret = poll(pfds, num_pfds, 16);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        wm_process_pending_a11y_actions();

        /* Accept new display clients */
        if (pfds[srv_pfd_idx].revents & POLLIN) {
            int cl_fd = accept(srv_fd, NULL, NULL);
            if (cl_fd >= 0) {
                int c_slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].active) {
                        c_slot = i;
                        break;
                    }
                }
                if (c_slot >= 0) {
                    fcntl(cl_fd, F_SETFL, fcntl(cl_fd, F_GETFL, 0) | O_NONBLOCK);
                    clients[c_slot].fd = cl_fd;
                    clients[c_slot].active = 1;
                    clients[c_slot].rx_len = 0;
                    peerinfo_t peer;
                    if (sock_peerinfo(cl_fd, &peer) == 0) {
                        clients[c_slot].pid = peer.pid;
                        clients[c_slot].agent_id = peer.agent_id;
                    } else {
                        clients[c_slot].pid = 0;
                        clients[c_slot].agent_id = 0;
                    }
                    clients[c_slot].is_privileged = 0;
                    detect_client_app_name(clients[c_slot].pid, clients[c_slot].app_name, sizeof(clients[c_slot].app_name));
                } else {
                    close(cl_fd);
                }
            }
        }

        /* Accept new control clients (wmctl) */
        if (pfds[ctrl_srv_pfd_idx].revents & POLLIN) {
            int ct_fd = accept(ctrl_fd, NULL, NULL);
            if (ct_fd >= 0) {
                int ct_slot = -1;
                for (int i = 0; i < MAX_CTRL_CLIENTS; i++) {
                    if (!ctrl_clients[i].active) {
                        ct_slot = i;
                        break;
                    }
                }
                if (ct_slot >= 0) {
                    fcntl(ct_fd, F_SETFL, fcntl(ct_fd, F_GETFL, 0) | O_NONBLOCK);
                    ctrl_clients[ct_slot].fd = ct_fd;
                    ctrl_clients[ct_slot].active = 1;
                    ctrl_clients[ct_slot].rx_len = 0;
                } else {
                    close(ct_fd);
                }
            }
        }

        /* Accept new approval client (intentd) */
        if (app_srv_pfd_idx >= 0 && (pfds[app_srv_pfd_idx].revents & POLLIN)) {
            int cl_fd = accept(g_approval_srv_fd, NULL, NULL);
            if (cl_fd >= 0) {
                peerinfo_t peer;
                if (sock_peerinfo(cl_fd, &peer) == 0 && peer.agent_id == 0) {
                    if (g_approval_cl_fd >= 0) close(g_approval_cl_fd);
                    fcntl(cl_fd, F_SETFL, fcntl(cl_fd, F_GETFL, 0) | O_NONBLOCK);
                    g_approval_cl_fd = cl_fd;
                } else {
                    close(cl_fd);
                }
            }
        }

        /* Handle approval messages from intentd */
        if (app_cl_pfd_idx >= 0 && (pfds[app_cl_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            struct approval_msg_header a_hdr;
            int n = read(g_approval_cl_fd, &a_hdr, sizeof(a_hdr));
            if (n == (int)sizeof(a_hdr)) {
                if (a_hdr.type == APPROVAL_MSG_REQUEST) {
                    struct approval_msg_request req;
                    if (read(g_approval_cl_fd, &req, sizeof(req)) == (int)sizeof(req)) {
                        g_approval.active = 1;
                        g_approval.approval_id = req.approval_id;
                        g_approval.agent_id = req.agent_id;
                        strncpy(g_approval.agent_name, req.agent_name, sizeof(g_approval.agent_name) - 1);
                        strncpy(g_approval.skill, req.skill, sizeof(g_approval.skill) - 1);
                        strncpy(g_approval.args_summary, req.args_summary, sizeof(g_approval.args_summary) - 1);
                        strncpy(g_approval.args_hash, req.args_hash, sizeof(g_approval.args_hash) - 1);
                        uint32_t t_ms = req.timeout_ms > 0 ? req.timeout_ms : 30000;
                        g_approval.expires_ms = get_now_ms() + t_ms;
                        g_approval.focused_btn = 0;
                        g_approval.intentd_fd = g_approval_cl_fd;
                        add_dirty_rect(0, 0, screen_w, screen_h);
                    }
                } else if (a_hdr.type == APPROVAL_MSG_CANCEL) {
                    struct approval_msg_cancel can;
                    if (read(g_approval_cl_fd, &can, sizeof(can)) == (int)sizeof(can)) {
                        if (g_approval.active && g_approval.approval_id == can.approval_id) {
                            g_approval.active = 0;
                            add_dirty_rect(0, 0, screen_w, screen_h);
                        }
                    }
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                close(g_approval_cl_fd);
                g_approval_cl_fd = -1;
            }
        }

        /* Mouse input */
        if (mouse_pfd_idx >= 0 && (pfds[mouse_pfd_idx].revents & POLLIN)) {
            struct input_event mev;
            while (read(ctx->mouse_fd, &mev, sizeof(mev)) == (int)sizeof(mev)) {
                uint64_t hw_now = get_now_ms();
                if (focused_window_id != 0) {
                    struct server_window* fw = find_window_by_id(focused_window_id);
                    if (fw) fw->last_hardware_input_time = hw_now;
                }
                if (mev.type == EV_REL) {
                    if (mev.code == REL_X) {
                        prev_mouse_x = mouse_x;
                        mouse_x += mev.value;
                        if (mouse_x < 0) mouse_x = 0;
                        if (mouse_x >= screen_w) mouse_x = screen_w - 1;
                        add_dirty_rect(prev_mouse_x - 16, prev_mouse_y - 16, 32, 32);
                        add_dirty_rect(mouse_x - 16, mouse_y - 16, 32, 32);
                        route_mouse_event(mouse_x, mouse_y, mev.value, 0, 0, 0, mouse_buttons, 0);
                    } else if (mev.code == REL_Y) {
                        prev_mouse_y = mouse_y;
                        mouse_y += mev.value;
                        if (mouse_y < 0) mouse_y = 0;
                        if (mouse_y >= screen_h) mouse_y = screen_h - 1;
                        add_dirty_rect(prev_mouse_x - 16, prev_mouse_y - 16, 32, 32);
                        add_dirty_rect(mouse_x - 16, mouse_y - 16, 32, 32);
                        route_mouse_event(mouse_x, mouse_y, 0, mev.value, 0, 0, mouse_buttons, 0);
                    } else if (mev.code == REL_WHEEL) {
                        route_mouse_event(mouse_x, mouse_y, 0, 0, 0, 0, mouse_buttons, mev.value);
                    }
                } else if (mev.type == EV_KEY) {
                    int btn = 0;
                    if (mev.code == BTN_LEFT) btn = 1;
                    else if (mev.code == BTN_RIGHT) btn = 2;
                    else if (mev.code == BTN_MIDDLE) btn = 3;

                    if (btn != 0) {
                        if (mev.value) mouse_buttons |= (1 << (btn - 1));
                        else mouse_buttons &= ~(1 << (btn - 1));

                        route_mouse_event(mouse_x, mouse_y, 0, 0, btn, mev.value, mouse_buttons, 0);
                    }
                }
            }
        }

        /* Keyboard input */
        if (kbd_pfd_idx >= 0 && (pfds[kbd_pfd_idx].revents & POLLIN)) {
            struct input_event kev;
            while (read(ctx->kbd_fd, &kev, sizeof(kev)) == (int)sizeof(kev)) {
                uint64_t hw_now = get_now_ms();
                if (focused_window_id != 0) {
                    struct server_window* fw = find_window_by_id(focused_window_id);
                    if (fw) fw->last_hardware_input_time = hw_now;
                }
                if (kev.type == EV_KEY) {
                    uint32_t mods = 0;
                    ioctl(ctx->kbd_fd, KBD_IOC_GETMODS, &mods);
                    uint32_t action = kev.value ? WIN_KEY_PRESS : WIN_KEY_RELEASE;
                    route_keyboard_event(kev.code, action, mods);
                }
            }
        }

        /* Active display clients */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int p_idx = client_pfd_map[i];
            if (p_idx >= 0 && (pfds[p_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                read_client_data(i);
            }
        }

        /* Active control clients (wmctl) */
        for (int i = 0; i < MAX_CTRL_CLIENTS; i++) {
            int p_idx = ctrl_pfd_map[i];
            if (p_idx >= 0 && (pfds[p_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                read_ctrl_data(i);
            }
        }

        /* Composite and flush to framebuffer */
        composite_screen();
    }

    printf("[wm] Shutting down window manager...\n");
    unlink(WIN_SOCK_PATH);
    unlink(WM_CTRL_SOCK_PATH);
    close(srv_fd);
    close(ctrl_fd);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            disconnect_client(i, "server shutdown");
        }
    }

    if (ctx) {
        gfx_close(ctx);
        ctx = NULL;
    }

    return 0;
}
