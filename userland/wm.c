/*
 * MyOS Window Manager & Compositor (wm.c) - Stage 18B-2 Minimal Compositor
 *
 * Implements:
 * - AF_UNIX stream socket server at /run/display.sock
 * - Display ownership via libgfx (KD_GRAPHICS + K_OFF)
 * - RAM back buffer composition with dirty rect blitting to /dev/fb0
 * - Software mouse cursor & input routing (window-relative pointer coordinates)
 * - Keyboard scancode to Unicode translation (/usr/share/keymaps/us.map)
 * - Strict client shm buffer validation (charset, size, fstat at commit)
 * - SIGBUS defense via sigsetjmp/siglongjmp against client shm truncation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include <sigsetjmp.h>
#include <gfx.h>
#include <win_protocol.h>

#define MAX_CLIENTS 32
#define MAX_WINDOWS 64
#define MAX_DIRTY_RECTS 64

struct rect {
    int x, y, w, h;
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
    uint32_t flags;
    uint32_t state;
    char title[128];

    struct client_buffer buffers[2];
    int active_buffer; /* 0 or 1, or -1 if none committed yet */

    int alive;
};

struct client {
    int fd;
    pid_t pid;
    int active;

    uint8_t rx_buf[WIN_MAX_MSG_LEN * 2];
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

static struct client clients[MAX_CLIENTS];
static struct server_window windows[MAX_WINDOWS];
static uint32_t z_order[MAX_WINDOWS];
static int num_z_windows = 0;
static uint32_t focused_window_id = 0;
static int hovered_window_idx = -1;

static struct rect dirty_rects[MAX_DIRTY_RECTS];
static int num_dirty_rects = 0;

/* Clipboard Storage */
static char g_clipboard_mime[32];
static char g_clipboard_data[1024];
static uint32_t g_clipboard_len = 0;

/* SIGBUS Resilience */
static sigjmp_buf g_bus_jmp;
static volatile sig_atomic_t g_in_client_blit = 0;
static int g_faulting_client = -1;
static volatile sig_atomic_t g_running = 1;

/* Keymap translation */
struct keymap_entry {
    uint32_t normal;
    uint32_t shift;
    uint32_t caps;
    uint32_t altgr;
};
static struct keymap_entry g_keymap[256];

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

static void init_fallback_keymap(void) {
    memset(g_keymap, 0, sizeof(g_keymap));

    /* Standard numbers */
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

    /* Letters */
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

    /* Punctuation */
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
        /* Cap reached: combine into full screen dirty rect */
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
            struct win_msg_focus foc;
            foc.focused = 0;
            send_client_msg(clients[old_win->client_idx].fd, WIN_MSG_FOCUS, old_win->id, &foc, sizeof(foc));
        }
    }

    focused_window_id = wid;

    if (focused_window_id != 0) {
        struct server_window* new_win = find_window_by_id(focused_window_id);
        if (new_win && new_win->alive) {
            raise_window(wid);
            struct win_msg_focus foc;
            foc.focused = 1;
            send_client_msg(clients[new_win->client_idx].fd, WIN_MSG_FOCUS, new_win->id, &foc, sizeof(foc));
            add_dirty_rect(new_win->x, new_win->y, new_win->width, new_win->height);
        }
    }
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

    /* Mark screen dirty where window was */
    add_dirty_rect(win->x, win->y, win->width, win->height);

    free_window_buffers(win);
    win->alive = 0;

    /* Remove from z_order */
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
        focused_window_id = (num_z_windows > 0) ? z_order[num_z_windows - 1] : 0;
        if (focused_window_id != 0) {
            struct server_window* top = find_window_by_id(focused_window_id);
            if (top) {
                struct win_msg_focus foc = { .focused = 1 };
                send_client_msg(clients[top->client_idx].fd, WIN_MSG_FOCUS, top->id, &foc, sizeof(foc));
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

    /* Destroy all windows owned by this client */
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
            return 0; /* Illegal character */
        }
    }
    return 1;
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

            /* Find free window slot */
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
            win->flags = cw.flags;
            win->state = WIN_STATE_NORMAL;
            memcpy(win->title, cw.title, sizeof(win->title) - 1);
            win->title[sizeof(win->title) - 1] = '\0';

            /* Default centered position with cascade offset */
            static int cascade = 0;
            win->x = (screen_w - (int)win->width) / 2 + (cascade % 8) * 20;
            win->y = (screen_h - (int)win->height) / 2 + (cascade % 8) * 20;
            cascade++;
            if (win->x < 0) win->x = 20;
            if (win->y < 20) win->y = 20;

            for (int b = 0; b < 2; b++) {
                win->buffers[b].fd = -1;
            }
            win->active_buffer = -1;
            win->alive = 1;

            /* Add to z-order and focus */
            if (num_z_windows < MAX_WINDOWS) {
                z_order[num_z_windows++] = win->id;
            }
            set_focused_window(win->id);
            add_dirty_rect(win->x, win->y, win->width, win->height);
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

            /* Cleanup previous mapping */
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
            if (win && win->client_idx == c_idx) {
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

            /* Re-fstat buffer to detect truncation before composition */
            struct stat st;
            if (fstat(buf->fd, &st) < 0 || (size_t)st.st_size < buf->size) {
                disconnect_client(c_idx, "shm file truncated at commit");
                return;
            }

            win->active_buffer = com.buffer_index;
            add_dirty_rect(win->x, win->y, win->width, win->height);

            /* Notify client with BUFFER_RELEASE for previous buffer */
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
            /* Client acknowledged resize */
            break;

        default:
            /* Unknown message from client */
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
            /* Incomplete message */
            break;
        }

        const uint8_t* payload = c->rx_buf + sizeof(hdr);
        uint32_t payload_len = hdr.len - sizeof(hdr);

        handle_client_message(c_idx, &hdr, payload, payload_len);
        if (!c->active) return; /* Client was disconnected by handler */

        size_t remaining = c->rx_len - hdr.len;
        if (remaining > 0) {
            memmove(c->rx_buf, c->rx_buf + hdr.len, remaining);
        }
        c->rx_len = remaining;
    }
}

static void route_mouse_event(int x, int y, int dx, int dy, int btn, int btn_state, int buttons, int wheel) {
    /* Determine which window is under pointer (top-to-bottom of z-order) */
    int hit_win_idx = -1;
    for (int i = num_z_windows - 1; i >= 0; i--) {
        uint32_t wid = z_order[i];
        for (int w = 0; w < MAX_WINDOWS; w++) {
            if (windows[w].alive && windows[w].id == wid) {
                if (x >= windows[w].x && x < windows[w].x + (int)windows[w].width &&
                    y >= windows[w].y && y < windows[w].y + (int)windows[w].height) {
                    hit_win_idx = w;
                    break;
                }
            }
        }
        if (hit_win_idx >= 0) break;
    }

    /* Pointer enter/leave dispatch */
    if (hit_win_idx != hovered_window_idx) {
        if (hovered_window_idx >= 0 && windows[hovered_window_idx].alive) {
            send_client_msg(clients[windows[hovered_window_idx].client_idx].fd,
                            WIN_MSG_POINTER_LEAVE, windows[hovered_window_idx].id, NULL, 0);
        }
        hovered_window_idx = hit_win_idx;
        if (hovered_window_idx >= 0 && windows[hovered_window_idx].alive) {
            struct win_msg_pointer_enter pe;
            pe.x = x - windows[hovered_window_idx].x;
            pe.y = y - windows[hovered_window_idx].y;
            send_client_msg(clients[windows[hovered_window_idx].client_idx].fd,
                            WIN_MSG_POINTER_ENTER, windows[hovered_window_idx].id, &pe, sizeof(pe));
        }
    }

    /* Button press handling & Click-to-focus/raise */
    if (btn != 0) {
        if (hit_win_idx >= 0 && windows[hit_win_idx].alive) {
            if (btn_state != 0) {
                set_focused_window(windows[hit_win_idx].id);
            }
            struct win_msg_pointer_button pb;
            pb.button = btn;
            pb.state = btn_state;
            pb.x = x - windows[hit_win_idx].x;
            pb.y = y - windows[hit_win_idx].y;
            send_client_msg(clients[windows[hit_win_idx].client_idx].fd,
                            WIN_MSG_POINTER_BUTTON, windows[hit_win_idx].id, &pb, sizeof(pb));
        } else {
            if (btn_state != 0) {
                set_focused_window(0);
            }
        }
    } else if (wheel != 0) {
        if (hit_win_idx >= 0 && windows[hit_win_idx].alive) {
            struct win_msg_pointer_wheel pw;
            pw.dx = 0;
            pw.dy = wheel;
            send_client_msg(clients[windows[hit_win_idx].client_idx].fd,
                            WIN_MSG_POINTER_WHEEL, windows[hit_win_idx].id, &pw, sizeof(pw));
        }
    } else {
        /* Pointer move */
        if (hit_win_idx >= 0 && windows[hit_win_idx].alive) {
            struct win_msg_pointer_move pm;
            pm.x = x - windows[hit_win_idx].x;
            pm.y = y - windows[hit_win_idx].y;
            send_client_msg(clients[windows[hit_win_idx].client_idx].fd,
                            WIN_MSG_POINTER_MOVE, windows[hit_win_idx].id, &pm, sizeof(pm));
        }
    }
}

static void route_keyboard_event(uint32_t keycode, uint32_t action, uint32_t mods) {
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

static void composite_screen(void) {
    if (num_dirty_rects == 0) return;

    for (int d = 0; d < num_dirty_rects; d++) {
        struct rect dr = dirty_rects[d];

        /* 1. Solid desktop background (#2A4D69 / 0xFF2A4D69) */
        uint32_t bg_color = 0xFF2A4D69;
        gfx_fill_rect(ctx->backbuffer, dr.x, dr.y, dr.w, dr.h, bg_color);

        /* 2. Composite windows in z-order */
        for (int z = 0; z < num_z_windows; z++) {
            struct server_window* win = find_window_by_id(z_order[z]);
            if (!win || !win->alive || win->active_buffer < 0) continue;

            struct client_buffer* buf = &win->buffers[win->active_buffer];
            if (!buf->valid || !buf->pixels) continue;

            /* Intersect window with dirty rect */
            int ix0 = (win->x > dr.x) ? win->x : dr.x;
            int iy0 = (win->y > dr.y) ? win->y : dr.y;
            int ix1 = ((win->x + (int)win->width) < (dr.x + dr.w)) ? (win->x + (int)win->width) : (dr.x + dr.w);
            int iy1 = ((win->y + (int)win->height) < (dr.y + dr.h)) ? (win->y + (int)win->height) : (dr.y + dr.h);

            if (ix1 <= ix0 || iy1 <= iy0) continue;

            int iw = ix1 - ix0;
            int ih = iy1 - iy0;
            int src_x = ix0 - win->x;
            int src_y = iy0 - win->y;

            /* Re-fstat before client buffer read */
            struct stat st;
            if (buf->fd < 0 || fstat(buf->fd, &st) < 0 || (size_t)st.st_size < buf->size) {
                disconnect_client(win->client_idx, "shm truncated at composite");
                continue;
            }

            /* SIGBUS protection with sigsetjmp */
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
        }

        /* 3. Draw cursor if within or near dirty rect */
        if (mouse_x >= dr.x - 16 && mouse_x < dr.x + dr.w + 16 &&
            mouse_y >= dr.y - 16 && mouse_y < dr.y + dr.h + 16) {
            gfx_draw_cursor(ctx->backbuffer, mouse_x, mouse_y);
        }

        /* 4. Flush dirty rect to framebuffer (rep movsd / memcpy) */
        gfx_flip_rect(ctx, dr.x, dr.y, dr.w, dr.h);
    }

    num_dirty_rects = 0;
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;

    printf("[wm] Initializing MyOS Window Manager & Compositor...\n");
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

    /* Load keymap */
    load_keymap("/usr/share/keymaps/us.map");

    /* Initialize libgfx context (takes KD_GRAPHICS and K_OFF) */
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

    /* Create AF_UNIX server socket */
    unlink(WIN_SOCK_PATH);
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("[wm] socket");
        gfx_close(ctx);
        return 1;
    }

    int flags = fcntl(srv_fd, F_GETFL, 0);
    fcntl(srv_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WIN_SOCK_PATH, sizeof(sun.sun_path) - 1);

    if (bind(srv_fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        perror("[wm] bind");
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }

    if (listen(srv_fd, 16) < 0) {
        perror("[wm] listen");
        close(srv_fd);
        gfx_close(ctx);
        return 1;
    }

    printf("[wm] Display server listening on %s (resolution %dx%d)\n", WIN_SOCK_PATH, screen_w, screen_h);
    fflush(stdout);

    /* Initial full screen repaint */
    add_dirty_rect(0, 0, screen_w, screen_h);
    composite_screen();

    /* Main Compositor & Event Loop */
    while (g_running) {
        struct pollfd pfds[MAX_CLIENTS + 3];
        int num_pfds = 0;

        /* 0: Mouse input */
        int mouse_pfd_idx = -1;
        if (ctx->mouse_fd >= 0) {
            mouse_pfd_idx = num_pfds;
            pfds[num_pfds].fd = ctx->mouse_fd;
            pfds[num_pfds].events = POLLIN;
            pfds[num_pfds].revents = 0;
            num_pfds++;
        }

        /* 1: Keyboard input */
        int kbd_pfd_idx = -1;
        if (ctx->kbd_fd >= 0) {
            kbd_pfd_idx = num_pfds;
            pfds[num_pfds].fd = ctx->kbd_fd;
            pfds[num_pfds].events = POLLIN;
            pfds[num_pfds].revents = 0;
            num_pfds++;
        }

        /* 2: Server listening socket */
        int srv_pfd_idx = num_pfds;
        pfds[num_pfds].fd = srv_fd;
        pfds[num_pfds].events = POLLIN;
        pfds[num_pfds].revents = 0;
        num_pfds++;

        /* 3+: Active client sockets */
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

        /* Frame pacing: 16ms poll timeout (~60 Hz) */
        int ret = poll(pfds, num_pfds, 16);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Check Server Socket: new connections */
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
                } else {
                    close(cl_fd);
                }
            }
        }

        /* Check Mouse Input */
        if (mouse_pfd_idx >= 0 && (pfds[mouse_pfd_idx].revents & POLLIN)) {
            struct input_event mev;
            while (read(ctx->mouse_fd, &mev, sizeof(mev)) == (int)sizeof(mev)) {
                if (mev.type == EV_REL) {
                    if (mev.code == REL_X) {
                        prev_mouse_x = mouse_x;
                        mouse_x += mev.value;
                        if (mouse_x < 0) mouse_x = 0;
                        if (mouse_x >= screen_w) mouse_x = screen_w - 1;
                        add_dirty_rect(prev_mouse_x, prev_mouse_y, 16, 16);
                        add_dirty_rect(mouse_x, mouse_y, 16, 16);
                        route_mouse_event(mouse_x, mouse_y, mev.value, 0, 0, 0, mouse_buttons, 0);
                    } else if (mev.code == REL_Y) {
                        prev_mouse_y = mouse_y;
                        mouse_y += mev.value;
                        if (mouse_y < 0) mouse_y = 0;
                        if (mouse_y >= screen_h) mouse_y = screen_h - 1;
                        add_dirty_rect(prev_mouse_x, prev_mouse_y, 16, 16);
                        add_dirty_rect(mouse_x, mouse_y, 16, 16);
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

        /* Check Keyboard Input */
        if (kbd_pfd_idx >= 0 && (pfds[kbd_pfd_idx].revents & POLLIN)) {
            struct input_event kev;
            while (read(ctx->kbd_fd, &kev, sizeof(kev)) == (int)sizeof(kev)) {
                if (kev.type == EV_KEY) {
                    uint32_t mods = 0;
                    ioctl(ctx->kbd_fd, KBD_IOC_GETMODS, &mods);
                    uint32_t action = kev.value ? WIN_KEY_PRESS : WIN_KEY_RELEASE;
                    route_keyboard_event(kev.code, action, mods);
                }
            }
        }

        /* Check Active Clients */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int p_idx = client_pfd_map[i];
            if (p_idx >= 0 && (pfds[p_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                read_client_data(i);
            }
        }

        /* Composite and flush damaged rects */
        composite_screen();
    }

    printf("[wm] Shutting down window manager...\n");
    unlink(WIN_SOCK_PATH);
    close(srv_fd);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            disconnect_client(i, "server shutdown");
        }
    }

    /* Restore console to text mode */
    if (ctx) {
        gfx_close(ctx);
        ctx = NULL;
    }

    return 0;
}
