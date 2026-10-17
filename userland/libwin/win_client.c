#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <win.h>
#include <win_protocol.h>

struct win_buffer_slot {
    int fd;
    char shm_name[64];
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t size;
    void* map;
    gfx_surface_t* surface;
};

struct win {
    uint32_t id;
    win_conn_t* conn;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    char title[128];

    struct win_buffer_slot buffers[2];
    int active_buffer; /* committed front buffer */
    int back_buffer;   /* back buffer for drawing */

    win_t* next;
};

struct win_conn {
    int sock_fd;
    pid_t pid;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t next_win_id;

    win_t* windows;

    /* Inbound ring/linear buffer */
    uint8_t rx_buf[WIN_MAX_MSG_LEN * 4];
    size_t rx_len;

    /* Clipboard cache */
    char clip_mime[32];
    char clip_data[1024];
    uint32_t clip_len;
    int clip_received;
};

static int send_msg(int fd, uint16_t type, uint32_t window_id, const void* payload, uint32_t payload_len) {
    if (payload_len + sizeof(struct win_msg_header) > WIN_MAX_MSG_LEN) {
        return -1;
    }
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

static int read_exact(int fd, void* dst, size_t count) {
    size_t received = 0;
    while (received < count) {
        int nr = read(fd, (char*)dst + received, count - received);
        if (nr <= 0) {
            if (nr < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        received += nr;
    }
    return 0;
}

static win_t* find_window(win_conn_t* conn, uint32_t wid) {
    win_t* w = conn->windows;
    while (w) {
        if (w->id == wid) return w;
        w = w->next;
    }
    return NULL;
}

static int alloc_buffer_slot(win_conn_t* conn, win_t* win, int slot_idx, uint32_t width, uint32_t height) {
    struct win_buffer_slot* b = &win->buffers[slot_idx];

    /* Unmap / clean up previous slot if present */
    if (b->map && b->size > 0) {
        munmap(b->map, b->size);
        b->map = NULL;
    }
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
    if (b->shm_name[0]) {
        char old_path[128];
        snprintf(old_path, sizeof(old_path), "/dev/shm/%s", b->shm_name);
        unlink(old_path);
    }
    if (b->surface) {
        free(b->surface);
        b->surface = NULL;
    }

    snprintf(b->shm_name, sizeof(b->shm_name), "win-%d-%d-%d", (int)conn->pid, (int)win->id, slot_idx);
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/dev/shm/%s", b->shm_name);
    unlink(full_path);

    int fd = open(full_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;

    uint32_t stride = width * 4;
    size_t size = stride * height;

    if (ftruncate(fd, size) != 0) {
        close(fd);
        unlink(full_path);
        return -1;
    }

    void* map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED || !map) {
        close(fd);
        unlink(full_path);
        return -1;
    }

    /* Zero initial buffer */
    memset(map, 0, size);

    gfx_surface_t* s = (gfx_surface_t*) calloc(1, sizeof(gfx_surface_t));
    if (!s) {
        munmap(map, size);
        close(fd);
        unlink(full_path);
        return -1;
    }

    s->width = width;
    s->height = height;
    s->pitch = stride;
    s->bpp = 4;
    s->pixels = (uint32_t*) map;
    s->owns_pixels = 0;
    gfx_reset_clip(s);

    b->fd = fd;
    b->width = width;
    b->height = height;
    b->stride = stride;
    b->size = size;
    b->map = map;
    b->surface = s;

    /* Attach to compositor */
    struct win_msg_attach_buffer att;
    memset(&att, 0, sizeof(att));
    att.buffer_index = slot_idx;
    strncpy(att.shm_name, b->shm_name, sizeof(att.shm_name) - 1);
    att.width = width;
    att.height = height;
    att.stride = stride;
    att.format = WIN_FORMAT_ARGB8888;

    if (send_msg(conn->sock_fd, WIN_MSG_ATTACH_BUFFER, win->id, &att, sizeof(att)) < 0) {
        return -1;
    }

    return 0;
}

win_conn_t* win_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WIN_SOCK_PATH, sizeof(sun.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        close(fd);
        return NULL;
    }

    win_conn_t* conn = (win_conn_t*) calloc(1, sizeof(win_conn_t));
    if (!conn) {
        close(fd);
        return NULL;
    }

    conn->sock_fd = fd;
    conn->pid = getpid();
    conn->next_win_id = 0;

    /* Handshake: send HELLO */
    struct win_msg_hello hello;
    hello.version = WIN_PROTO_VERSION;
    hello.pid = (uint32_t) conn->pid;

    if (send_msg(fd, WIN_MSG_HELLO, 0, &hello, sizeof(hello)) < 0) {
        close(fd);
        free(conn);
        return NULL;
    }

    /* Read HELLO_ACK */
    struct win_msg_header hdr;
    if (read_exact(fd, &hdr, sizeof(hdr)) < 0) {
        close(fd);
        free(conn);
        return NULL;
    }

    if (hdr.type != WIN_MSG_HELLO_ACK || hdr.len < sizeof(hdr) + sizeof(struct win_msg_hello_ack)) {
        close(fd);
        free(conn);
        return NULL;
    }

    struct win_msg_hello_ack ack;
    if (read_exact(fd, &ack, sizeof(ack)) < 0) {
        close(fd);
        free(conn);
        return NULL;
    }

    /* Discard any trailing padding in HELLO_ACK */
    size_t extra = hdr.len - (sizeof(hdr) + sizeof(ack));
    if (extra > 0) {
        char tmp[128];
        while (extra > 0) {
            size_t take = (extra > sizeof(tmp)) ? sizeof(tmp) : extra;
            read_exact(fd, tmp, take);
            extra -= take;
        }
    }

    conn->screen_w = ack.screen_w;
    conn->screen_h = ack.screen_h;

    return conn;
}

void win_disconnect(win_conn_t* conn) {
    if (!conn) return;

    while (conn->windows) {
        win_destroy(conn->windows);
    }

    if (conn->sock_fd >= 0) {
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }
    free(conn);
}

int win_fd(win_conn_t* conn) {
    return conn ? conn->sock_fd : -1;
}

uint32_t win_screen_width(win_conn_t* conn) {
    return conn ? conn->screen_w : 0;
}

uint32_t win_screen_height(win_conn_t* conn) {
    return conn ? conn->screen_h : 0;
}

win_t* win_create(win_conn_t* conn, int width, int height, uint32_t flags, const char* title) {
    if (!conn || width <= 0 || height <= 0) return NULL;

    win_t* win = (win_t*) calloc(1, sizeof(win_t));
    if (!win) return NULL;

    win->id = ++conn->next_win_id;
    win->conn = conn;
    win->width = width;
    win->height = height;
    win->flags = flags;
    if (title) {
        strncpy(win->title, title, sizeof(win->title) - 1);
    }

    for (int i = 0; i < 2; i++) {
        win->buffers[i].fd = -1;
    }

    /* Send CREATE_WINDOW */
    struct win_msg_create_window cw;
    memset(&cw, 0, sizeof(cw));
    cw.width = width;
    cw.height = height;
    cw.flags = flags;
    if (title) {
        strncpy(cw.title, title, sizeof(cw.title) - 1);
    }

    if (send_msg(conn->sock_fd, WIN_MSG_CREATE_WINDOW, win->id, &cw, sizeof(cw)) < 0) {
        free(win);
        return NULL;
    }

    /* Allocate buffer slot 0 and 1 */
    if (alloc_buffer_slot(conn, win, 0, width, height) < 0 ||
        alloc_buffer_slot(conn, win, 1, width, height) < 0) {
        win_destroy(win);
        return NULL;
    }

    win->active_buffer = 0;
    win->back_buffer = 0;

    /* Add to connection window list */
    win->next = conn->windows;
    conn->windows = win;

    return win;
}

void win_destroy(win_t* win) {
    if (!win || !win->conn) return;
    win_conn_t* conn = win->conn;

    send_msg(conn->sock_fd, WIN_MSG_DESTROY_WINDOW, win->id, NULL, 0);

    for (int i = 0; i < 2; i++) {
        struct win_buffer_slot* b = &win->buffers[i];
        if (b->map && b->size > 0) {
            munmap(b->map, b->size);
            b->map = NULL;
        }
        if (b->fd >= 0) {
            close(b->fd);
            b->fd = -1;
        }
        if (b->shm_name[0]) {
            char path[128];
            snprintf(path, sizeof(path), "/dev/shm/%s", b->shm_name);
            unlink(path);
        }
        if (b->surface) {
            free(b->surface);
            b->surface = NULL;
        }
    }

    /* Remove from list */
    if (conn->windows == win) {
        conn->windows = win->next;
    } else {
        win_t* cur = conn->windows;
        while (cur && cur->next != win) {
            cur = cur->next;
        }
        if (cur) cur->next = win->next;
    }

    free(win);
}

uint32_t win_id(win_t* win) {
    return win ? win->id : 0;
}

void win_set_title(win_t* win, const char* title) {
    if (!win || !title) return;
    strncpy(win->title, title, sizeof(win->title) - 1);
    struct win_msg_set_title st;
    memset(&st, 0, sizeof(st));
    strncpy(st.title, title, sizeof(st.title) - 1);
    send_msg(win->conn->sock_fd, WIN_MSG_SET_TITLE, win->id, &st, sizeof(st));
}

void win_set_min_max_size(win_t* win, int min_w, int min_h, int max_w, int max_h) {
    if (!win) return;
    struct win_msg_set_min_max_size mm;
    mm.min_w = (uint32_t) min_w;
    mm.min_h = (uint32_t) min_h;
    mm.max_w = (uint32_t) max_w;
    mm.max_h = (uint32_t) max_h;
    send_msg(win->conn->sock_fd, WIN_MSG_SET_MIN_MAX_SIZE, win->id, &mm, sizeof(mm));
}

void win_set_state(win_t* win, uint32_t state) {
    if (!win) return;
    struct win_msg_set_state ss;
    ss.state = state;
    send_msg(win->conn->sock_fd, WIN_MSG_SET_STATE, win->id, &ss, sizeof(ss));
}

void win_request_move(win_t* win) {
    if (!win) return;
    send_msg(win->conn->sock_fd, WIN_MSG_REQUEST_MOVE, win->id, NULL, 0);
}

int win_width(win_t* win) {
    return win ? (int)win->width : 0;
}

int win_height(win_t* win) {
    return win ? (int)win->height : 0;
}

gfx_surface_t* win_buffer(win_t* win) {
    if (!win) return NULL;
    return win->buffers[win->back_buffer].surface;
}

void win_damage(win_t* win, int x, int y, int w, int h) {
    if (!win) return;
    struct win_msg_damage dmg;
    dmg.x = x;
    dmg.y = y;
    dmg.width = w;
    dmg.height = h;
    send_msg(win->conn->sock_fd, WIN_MSG_DAMAGE, win->id, &dmg, sizeof(dmg));
}

void win_commit(win_t* win) {
    if (!win) return;
    struct win_msg_commit c;
    c.buffer_index = win->back_buffer;
    send_msg(win->conn->sock_fd, WIN_MSG_COMMIT, win->id, &c, sizeof(c));

    win->active_buffer = win->back_buffer;
    win->back_buffer = 1 - win->back_buffer;
}

static int parse_next_message(win_conn_t* conn, win_event_t* event) {
    if (conn->rx_len < sizeof(struct win_msg_header)) {
        return 0;
    }

    struct win_msg_header hdr;
    memcpy(&hdr, conn->rx_buf, sizeof(hdr));

    if (hdr.len < sizeof(hdr) || hdr.len > WIN_MAX_MSG_LEN) {
        /* Fatal framing violation */
        return -1;
    }

    if (conn->rx_len < hdr.len) {
        /* Incomplete message, wait for more bytes */
        return 0;
    }

    const uint8_t* payload = conn->rx_buf + sizeof(hdr);
    uint32_t payload_len = hdr.len - sizeof(hdr);

    memset(event, 0, sizeof(*event));
    event->window = find_window(conn, hdr.window_id);

    int event_ready = 0;

    switch (hdr.type) {
        case WIN_MSG_CONFIGURE: {
            if (payload_len >= sizeof(struct win_msg_configure)) {
                struct win_msg_configure cfg;
                memcpy(&cfg, payload, sizeof(cfg));

                win_t* win = event->window;
                if (win) {
                    if (cfg.width != win->width || cfg.height != win->height) {
                        alloc_buffer_slot(conn, win, 0, cfg.width, cfg.height);
                        alloc_buffer_slot(conn, win, 1, cfg.width, cfg.height);
                        win->width = cfg.width;
                        win->height = cfg.height;
                        win->active_buffer = 0;
                        win->back_buffer = 0;
                    }
                    send_msg(conn->sock_fd, WIN_MSG_CONFIGURE_ACK, win->id, NULL, 0);
                }

                event->type = WIN_EVENT_CONFIGURE;
                event->configure.width = cfg.width;
                event->configure.height = cfg.height;
                event->configure.state = cfg.state;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_KEY: {
            if (payload_len >= sizeof(struct win_msg_key)) {
                struct win_msg_key k;
                memcpy(&k, payload, sizeof(k));
                event->type = WIN_EVENT_KEY;
                event->key.keycode = k.keycode;
                event->key.unicode = k.unicode;
                event->key.modifiers = k.modifiers;
                event->key.action = k.action;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_POINTER_ENTER: {
            if (payload_len >= sizeof(struct win_msg_pointer_enter)) {
                struct win_msg_pointer_enter pe;
                memcpy(&pe, payload, sizeof(pe));
                event->type = WIN_EVENT_POINTER_ENTER;
                event->pointer.x = pe.x;
                event->pointer.y = pe.y;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_POINTER_LEAVE: {
            event->type = WIN_EVENT_POINTER_LEAVE;
            event_ready = 1;
            break;
        }

        case WIN_MSG_POINTER_MOVE: {
            if (payload_len >= sizeof(struct win_msg_pointer_move)) {
                struct win_msg_pointer_move pm;
                memcpy(&pm, payload, sizeof(pm));
                event->type = WIN_EVENT_POINTER_MOVE;
                event->pointer.x = pm.x;
                event->pointer.y = pm.y;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_POINTER_BUTTON: {
            if (payload_len >= sizeof(struct win_msg_pointer_button)) {
                struct win_msg_pointer_button pb;
                memcpy(&pb, payload, sizeof(pb));
                event->type = WIN_EVENT_POINTER_BUTTON;
                event->pointer.button = pb.button;
                event->pointer.state = pb.state;
                event->pointer.x = pb.x;
                event->pointer.y = pb.y;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_POINTER_WHEEL: {
            if (payload_len >= sizeof(struct win_msg_pointer_wheel)) {
                struct win_msg_pointer_wheel pw;
                memcpy(&pw, payload, sizeof(pw));
                event->type = WIN_EVENT_POINTER_WHEEL;
                event->pointer.dx = pw.dx;
                event->pointer.dy = pw.dy;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_FOCUS: {
            if (payload_len >= sizeof(struct win_msg_focus)) {
                struct win_msg_focus f;
                memcpy(&f, payload, sizeof(f));
                event->type = WIN_EVENT_FOCUS;
                event->focus.focused = f.focused;
                event_ready = 1;
            }
            break;
        }

        case WIN_MSG_CLOSE_REQUEST: {
            event->type = WIN_EVENT_CLOSE_REQUEST;
            event_ready = 1;
            break;
        }

        case WIN_MSG_FRAME_DONE: {
            event->type = WIN_EVENT_FRAME_DONE;
            event_ready = 1;
            break;
        }

        case WIN_MSG_CLIPBOARD_DATA: {
            if (payload_len >= sizeof(struct win_msg_clipboard_data)) {
                struct win_msg_clipboard_data cd;
                memcpy(&cd, payload, sizeof(cd));
                memset(conn->clip_mime, 0, sizeof(conn->clip_mime));
                memcpy(conn->clip_mime, cd.mime, sizeof(conn->clip_mime) - 1);
                conn->clip_len = (cd.len < sizeof(conn->clip_data)) ? cd.len : sizeof(conn->clip_data);
                memcpy(conn->clip_data, cd.data, conn->clip_len);
                conn->clip_received = 1;
            }
            break;
        }

        default:
            /* Ignore unrecognized / server-internal messages */
            break;
    }

    /* Shift remaining rx_buf */
    size_t remaining = conn->rx_len - hdr.len;
    if (remaining > 0) {
        memmove(conn->rx_buf, conn->rx_buf + hdr.len, remaining);
    }
    conn->rx_len = remaining;

    return event_ready ? 1 : 2; /* 1 = event ready, 2 = consumed but no external event */
}

int win_event_poll(win_conn_t* conn, win_event_t* event) {
    return win_event_wait(conn, event, 0);
}

int win_event_wait(win_conn_t* conn, win_event_t* event, int timeout_ms) {
    if (!conn || !event) return -1;

    while (1) {
        /* Check if we already have a parsed event in the rx buffer */
        int r = parse_next_message(conn, event);
        if (r == 1) return 1;
        if (r < 0) return -1; /* Framing error */
        if (r == 2) continue; /* Consumed internal message, try next */

        /* Need more bytes from socket */
        struct pollfd pfd;
        pfd.fd = conn->sock_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            /* Timeout */
            return 0;
        }

        size_t space = sizeof(conn->rx_buf) - conn->rx_len;
        if (space == 0) {
            /* Buffer overflow */
            return -1;
        }

        int nr = read(conn->sock_fd, conn->rx_buf + conn->rx_len, space);
        if (nr <= 0) {
            if (nr < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1; /* Connection closed */
        }
        conn->rx_len += nr;
    }
}

void win_clipboard_set(win_conn_t* conn, const char* mime, const void* data, uint32_t len) {
    if (!conn || !mime || !data) return;
    struct win_msg_clipboard_set cs;
    memset(&cs, 0, sizeof(cs));
    strncpy(cs.mime, mime, sizeof(cs.mime) - 1);
    cs.len = (len < sizeof(cs.data)) ? len : sizeof(cs.data);
    memcpy(cs.data, data, cs.len);
    send_msg(conn->sock_fd, WIN_MSG_CLIPBOARD_SET, 0, &cs, sizeof(cs));
}

int win_clipboard_get(win_conn_t* conn, const char* mime, void* buf, uint32_t max_len) {
    if (!conn || !mime || !buf || max_len == 0) return -1;
    conn->clip_received = 0;
    struct win_msg_clipboard_get cg;
    memset(&cg, 0, sizeof(cg));
    strncpy(cg.mime, mime, sizeof(cg.mime) - 1);
    if (send_msg(conn->sock_fd, WIN_MSG_CLIPBOARD_GET, 0, &cg, sizeof(cg)) < 0) {
        return -1;
    }

    /* Wait up to 200ms for response */
    win_event_t ev;
    int waited = 0;
    while (!conn->clip_received && waited < 200) {
        int r = win_event_wait(conn, &ev, 20);
        if (r < 0) return -1;
        waited += 20;
    }

    if (!conn->clip_received) return -1;

    uint32_t to_copy = (conn->clip_len < max_len) ? conn->clip_len : max_len;
    memcpy(buf, conn->clip_data, to_copy);
    return (int) to_copy;
}
