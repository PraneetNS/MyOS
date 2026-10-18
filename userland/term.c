/*
 * term.c - Graphical Terminal Emulator for MyOS
 *
 * Stage 18B-2 Step 4
 * Features:
 * - VT100 / xterm-256color escape sequence parser
 * - Spawns shell via forkpty() with TERM=xterm-256color
 * - 80x25 character grid with 8x16 bitmap font
 * - 2000-line scrollback buffer with mouse wheel / PgUp / PgDn navigation
 * - ANSI 16-color palette with bold and reverse video
 * - Full cursor movement and editing sequences (CUP, ED, EL, SGR)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <pty.h>
#include <gfx.h>
#include <win.h>

#define TERM_COLS 80
#define TERM_ROWS 25
#define CHAR_W 8
#define CHAR_H 16
#define SCROLLBACK_MAX 2000

#define DEFAULT_BG 0xFF0F172A /* Slate Dark #0F172A */
#define DEFAULT_FG 0xFFF8FAFC /* Near White #F8FAFC */

/* ANSI 16 Colors */
static const uint32_t s_ansi_colors[16] = {
    0xFF1E293B, /* 0: Black / Dark Slate */
    0xFFEF4444, /* 1: Red */
    0xFF10B981, /* 2: Green */
    0xFFF59E0B, /* 3: Yellow */
    0xFF3B82F6, /* 4: Blue */
    0xFFA855F7, /* 5: Magenta */
    0xFF06B6D4, /* 6: Cyan */
    0xFFE2E8F0, /* 7: Light Gray */

    0xFF64748B, /* 8: Bright Black (Gray) */
    0xFFF87171, /* 9: Bright Red */
    0xFF34D399, /* 10: Bright Green */
    0xFFFBBF24, /* 11: Bright Yellow */
    0xFF60A5FA, /* 12: Bright Blue */
    0xFFC084FC, /* 13: Bright Magenta */
    0xFF22D3EE, /* 14: Bright Cyan */
    0xFFFFFFFF  /* 15: Bright White */
};

typedef struct {
    char ch;
    uint32_t fg;
    uint32_t bg;
    uint8_t bold;
    uint8_t reverse;
} term_cell_t;

static term_cell_t s_grid[SCROLLBACK_MAX][TERM_COLS];
static int s_total_lines = TERM_ROWS;
static int s_scroll_offset = 0; /* 0 = bottom, >0 = viewing history */

static int s_cursor_x = 0;
static int s_cursor_y = 0;
static int s_saved_cx = 0;
static int s_saved_cy = 0;
static int s_cursor_visible = 1;

static uint32_t s_cur_fg = DEFAULT_FG;
static uint32_t s_cur_bg = DEFAULT_BG;
static uint8_t  s_cur_bold = 0;
static uint8_t  s_cur_reverse = 0;

/* Escape parser state */
enum {
    STATE_NORMAL = 0,
    STATE_ESC,
    STATE_CSI
};
static int s_parse_state = STATE_NORMAL;
static char s_csi_buf[64];
static int s_csi_len = 0;

static void clear_cell(term_cell_t* cell) {
    cell->ch = ' ';
    cell->fg = s_cur_fg;
    cell->bg = s_cur_bg;
    cell->bold = 0;
    cell->reverse = 0;
}

static void clear_row(int row_idx) {
    if (row_idx < 0 || row_idx >= SCROLLBACK_MAX) return;
    for (int col = 0; col < TERM_COLS; col++) {
        clear_cell(&s_grid[row_idx][col]);
    }
}

static void init_grid(void) {
    for (int r = 0; r < SCROLLBACK_MAX; r++) {
        clear_row(r);
    }
    s_total_lines = TERM_ROWS;
    s_scroll_offset = 0;
    s_cursor_x = 0;
    s_cursor_y = 0;
}

static int get_screen_row(int y) {
    int top_line = s_total_lines - TERM_ROWS - s_scroll_offset;
    if (top_line < 0) top_line = 0;
    return top_line + y;
}

static void scroll_up(void) {
    if (s_total_lines < SCROLLBACK_MAX) {
        s_total_lines++;
        clear_row(s_total_lines - 1);
    } else {
        /* Shift scrollback up */
        memmove(&s_grid[0], &s_grid[1], sizeof(s_grid[0]) * (SCROLLBACK_MAX - 1));
        clear_row(SCROLLBACK_MAX - 1);
    }
    s_scroll_offset = 0;
}

static void handle_sgr(int param) {
    if (param == 0) {
        /* Reset */
        s_cur_fg = DEFAULT_FG;
        s_cur_bg = DEFAULT_BG;
        s_cur_bold = 0;
        s_cur_reverse = 0;
    } else if (param == 1) {
        s_cur_bold = 1;
    } else if (param == 7) {
        s_cur_reverse = 1;
    } else if (param >= 30 && param <= 37) {
        s_cur_fg = s_ansi_colors[param - 30 + (s_cur_bold ? 8 : 0)];
    } else if (param == 39) {
        s_cur_fg = DEFAULT_FG;
    } else if (param >= 40 && param <= 47) {
        s_cur_bg = s_ansi_colors[param - 40];
    } else if (param == 49) {
        s_cur_bg = DEFAULT_BG;
    } else if (param >= 90 && param <= 97) {
        s_cur_fg = s_ansi_colors[param - 90 + 8];
    } else if (param >= 100 && param <= 107) {
        s_cur_bg = s_ansi_colors[param - 100 + 8];
    }
}

static void parse_csi(char cmd) {
    int params[8] = { 0 };
    int nparams = 0;

    char* p = s_csi_buf;
    if (*p == '?') p++; /* Private mode e.g. ?25h */

    char* token = strtok(p, ";");
    while (token && nparams < 8) {
        params[nparams++] = atoi(token);
        token = strtok(NULL, ";");
    }

    switch (cmd) {
        case 'm': { /* SGR */
            if (nparams == 0) handle_sgr(0);
            else {
                for (int i = 0; i < nparams; i++) handle_sgr(params[i]);
            }
            break;
        }

        case 'H':
        case 'f': { /* CUP - Cursor Position */
            int row = (nparams > 0 && params[0] > 0) ? params[0] - 1 : 0;
            int col = (nparams > 1 && params[1] > 0) ? params[1] - 1 : 0;
            if (row < 0) row = 0; if (row >= TERM_ROWS) row = TERM_ROWS - 1;
            if (col < 0) col = 0; if (col >= TERM_COLS) col = TERM_COLS - 1;
            s_cursor_y = row;
            s_cursor_x = col;
            break;
        }

        case 'A': { /* CUU - Cursor Up */
            int n = (nparams > 0 && params[0] > 0) ? params[0] : 1;
            s_cursor_y -= n;
            if (s_cursor_y < 0) s_cursor_y = 0;
            break;
        }

        case 'B': { /* CUD - Cursor Down */
            int n = (nparams > 0 && params[0] > 0) ? params[0] : 1;
            s_cursor_y += n;
            if (s_cursor_y >= TERM_ROWS) s_cursor_y = TERM_ROWS - 1;
            break;
        }

        case 'C': { /* CUF - Cursor Forward */
            int n = (nparams > 0 && params[0] > 0) ? params[0] : 1;
            s_cursor_x += n;
            if (s_cursor_x >= TERM_COLS) s_cursor_x = TERM_COLS - 1;
            break;
        }

        case 'D': { /* CUB - Cursor Backward */
            int n = (nparams > 0 && params[0] > 0) ? params[0] : 1;
            s_cursor_x -= n;
            if (s_cursor_x < 0) s_cursor_x = 0;
            break;
        }

        case 'J': { /* ED - Erase Display */
            int mode = (nparams > 0) ? params[0] : 0;
            if (mode == 2) {
                /* Clear whole screen */
                for (int y = 0; y < TERM_ROWS; y++) {
                    clear_row(get_screen_row(y));
                }
                s_cursor_x = 0;
                s_cursor_y = 0;
            }
            break;
        }

        case 'K': { /* EL - Erase Line */
            int mode = (nparams > 0) ? params[0] : 0;
            int r = get_screen_row(s_cursor_y);
            if (mode == 0) {
                for (int c = s_cursor_x; c < TERM_COLS; c++) clear_cell(&s_grid[r][c]);
            } else if (mode == 1) {
                for (int c = 0; c <= s_cursor_x && c < TERM_COLS; c++) clear_cell(&s_grid[r][c]);
            } else if (mode == 2) {
                clear_row(r);
            }
            break;
        }

        case 'h': /* Mode Set */
            if (strcmp(s_csi_buf, "?25") == 0) s_cursor_visible = 1;
            break;

        case 'l': /* Mode Reset */
            if (strcmp(s_csi_buf, "?25") == 0) s_cursor_visible = 0;
            break;

        default:
            break;
    }
}

static void term_put_char(char c) {
    if (s_parse_state == STATE_NORMAL) {
        if (c == '\033') {
            s_parse_state = STATE_ESC;
            return;
        }
        if (c == '\r') {
            s_cursor_x = 0;
            return;
        }
        if (c == '\n') {
            s_cursor_x = 0;
            s_cursor_y++;
            if (s_cursor_y >= TERM_ROWS) {
                s_cursor_y = TERM_ROWS - 1;
                scroll_up();
            }
            return;
        }
        if (c == '\b') {
            if (s_cursor_x > 0) s_cursor_x--;
            return;
        }
        if (c == '\t') {
            s_cursor_x = (s_cursor_x + 8) & ~7;
            if (s_cursor_x >= TERM_COLS) s_cursor_x = TERM_COLS - 1;
            return;
        }

        /* Printable characters */
        if ((unsigned char)c >= 32) {
            int r = get_screen_row(s_cursor_y);
            s_grid[r][s_cursor_x].ch = c;
            s_grid[r][s_cursor_x].fg = s_cur_fg;
            s_grid[r][s_cursor_x].bg = s_cur_bg;
            s_grid[r][s_cursor_x].bold = s_cur_bold;
            s_grid[r][s_cursor_x].reverse = s_cur_reverse;

            s_cursor_x++;
            if (s_cursor_x >= TERM_COLS) {
                s_cursor_x = 0;
                s_cursor_y++;
                if (s_cursor_y >= TERM_ROWS) {
                    s_cursor_y = TERM_ROWS - 1;
                    scroll_up();
                }
            }
        }
    } else if (s_parse_state == STATE_ESC) {
        if (c == '[') {
            s_parse_state = STATE_CSI;
            s_csi_len = 0;
            s_csi_buf[0] = '\0';
        } else if (c == '7') { /* Save cursor */
            s_saved_cx = s_cursor_x;
            s_saved_cy = s_cursor_y;
            s_parse_state = STATE_NORMAL;
        } else if (c == '8') { /* Restore cursor */
            s_cursor_x = s_saved_cx;
            s_cursor_y = s_saved_cy;
            s_parse_state = STATE_NORMAL;
        } else {
            s_parse_state = STATE_NORMAL;
        }
    } else if (s_parse_state == STATE_CSI) {
        if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
            if (s_csi_len < (int)sizeof(s_csi_buf) - 2) {
                s_csi_buf[s_csi_len++] = c;
                s_csi_buf[s_csi_len] = '\0';
            }
        } else {
            parse_csi(c);
            s_parse_state = STATE_NORMAL;
        }
    }
}

static void render_terminal(gfx_surface_t* s) {
    if (!s) return;

    for (int y = 0; y < TERM_ROWS; y++) {
        int r = get_screen_row(y);
        for (int x = 0; x < TERM_COLS; x++) {
            term_cell_t* cell = &s_grid[r][x];
            uint32_t fg = cell->reverse ? cell->bg : cell->fg;
            uint32_t bg = cell->reverse ? cell->fg : cell->bg;
            gfx_draw_char(s, x * CHAR_W, y * CHAR_H, cell->ch ? cell->ch : ' ', fg, bg);
        }
    }

    /* Draw cursor block */
    if (s_cursor_visible && s_scroll_offset == 0) {
        int cx = s_cursor_x * CHAR_W;
        int cy = s_cursor_y * CHAR_H;
        gfx_draw_rect(s, cx, cy, CHAR_W, CHAR_H, 0xFF38BDF8);
        gfx_draw_rect(s, cx + 1, cy + 1, CHAR_W - 2, CHAR_H - 2, 0xFF38BDF8);
    }
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;

    printf("[term] Starting MyOS Terminal Emulator...\n");
    fflush(stdout);

    int win_w = TERM_COLS * CHAR_W;
    int win_h = TERM_ROWS * CHAR_H;

    win_conn_t* conn = win_connect();
    if (!conn) {
        fprintf(stderr, "[term] Error: cannot connect to display server\n");
        return 1;
    }

    win_t* win = win_create(conn, win_w, win_h, 0, "Terminal (xterm-256color)");
    if (!win) {
        fprintf(stderr, "[term] Error: cannot create window\n");
        win_disconnect(conn);
        return 1;
    }

    init_grid();

    /* Spawn child shell via forkpty() */
    int pty_master = -1;
    struct winsize ws;
    ws.ws_col = TERM_COLS;
    ws.ws_row = TERM_ROWS;
    ws.ws_xpixel = win_w;
    ws.ws_ypixel = win_h;

    signal(SIGPIPE, SIG_IGN);

    pid_t child_pid = forkpty(&pty_master, NULL, NULL, &ws);
    if (child_pid < 0) {
        perror("[term] forkpty failed");
        win_disconnect(conn);
        return 1;
    }

    if (child_pid == 0) {
        /* Child Process: Shell */
        for (int fd = 3; fd < 256; fd++) {
            close(fd);
        }
        setenv("TERM", "xterm-256color", 1);
        char* sh_argv[] = { "/bin/sh.elf", NULL };
        execv("/bin/sh.elf", sh_argv);
        execv("/bin/sh", sh_argv);
        _exit(1);
    }

    /* Set non-blocking on PTY master */
    fcntl(pty_master, F_SETFL, fcntl(pty_master, F_GETFL, 0) | O_NONBLOCK);

    printf("[term] Child shell running (PID %d, PTY master %d)\n", (int)child_pid, pty_master);
    fflush(stdout);

    /* Initial draw */
    gfx_surface_t* s = win_buffer(win);
    render_terminal(s);
    win_damage(win, 0, 0, win_w, win_h);
    win_commit(win);

    int running = 1;
    int win_sock_fd = win_fd(conn);

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd = win_sock_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        pfds[1].fd = pty_master;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int ret = poll(pfds, 2, 16);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfds[0].revents & (POLLHUP | POLLERR)) {
            running = 0;
            break;
        }

        int dirty = 0;

        /* Check PTY output from shell */
        if (pfds[1].revents & POLLIN) {
            char pty_buf[1024];
            int n = read(pty_master, pty_buf, sizeof(pty_buf));
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    term_put_char(pty_buf[i]);
                }
                dirty = 1;
            } else if (n == 0) {
                /* Shell exited */
                running = 0;
                break;
            }
        }

        if (pfds[1].revents & (POLLHUP | POLLERR)) {
            running = 0;
            break;
        }

        /* Check Window Events */
        win_event_t ev;
        while (win_event_poll(conn, &ev) > 0) {
            if (ev.type == WIN_EVENT_CLOSE_REQUEST) {
                running = 0;
                break;
            } else if (ev.type == WIN_EVENT_KEY && (ev.key.action == WIN_KEY_PRESS || ev.key.action == WIN_KEY_REPEAT)) {
                uint32_t kc = ev.key.keycode;
                uint32_t uc = ev.key.unicode;

                if (kc == 28) { /* Enter */
                    write(pty_master, "\n", 1);
                } else if (kc == 14) { /* Backspace */
                    write(pty_master, "\x7f", 1);
                } else if (kc == 103) { /* Up Arrow */
                    write(pty_master, "\033[A", 3);
                } else if (kc == 108) { /* Down Arrow */
                    write(pty_master, "\033[B", 3);
                } else if (kc == 106) { /* Right Arrow */
                    write(pty_master, "\033[C", 3);
                } else if (kc == 105) { /* Left Arrow */
                    write(pty_master, "\033[D", 3);
                } else if (kc == 102) { /* Home */
                    write(pty_master, "\033[H", 3);
                } else if (kc == 107) { /* End */
                    write(pty_master, "\033[F", 3);
                } else if (kc == 104) { /* Page Up */
                    s_scroll_offset += 10;
                    if (s_scroll_offset > s_total_lines - TERM_ROWS) {
                        s_scroll_offset = s_total_lines - TERM_ROWS;
                    }
                    if (s_scroll_offset < 0) s_scroll_offset = 0;
                    dirty = 1;
                } else if (kc == 109) { /* Page Down */
                    s_scroll_offset -= 10;
                    if (s_scroll_offset < 0) s_scroll_offset = 0;
                    dirty = 1;
                } else if (uc >= 32 && uc < 127) {
                    char ch = (char)uc;
                    write(pty_master, &ch, 1);
                }
            } else if (ev.type == WIN_EVENT_POINTER_WHEEL) {
                if (ev.pointer.wheel > 0) {
                    s_scroll_offset += 3;
                    if (s_scroll_offset > s_total_lines - TERM_ROWS) {
                        s_scroll_offset = s_total_lines - TERM_ROWS;
                    }
                } else if (ev.pointer.wheel < 0) {
                    s_scroll_offset -= 3;
                    if (s_scroll_offset < 0) s_scroll_offset = 0;
                }
                dirty = 1;
            }
        }

        if (dirty) {
            s = win_buffer(win);
            render_terminal(s);
            win_damage(win, 0, 0, win_w, win_h);
            win_commit(win);
        }
    }

    printf("[term] Cleaning up terminal emulator...\n");
    fflush(stdout);

    if (pty_master >= 0) {
        close(pty_master);
        pty_master = -1;
    }
    if (child_pid > 0) {
        kill(child_pid, SIGHUP);
        int st = 0;
        waitpid(child_pid, &st, 0);
    }
    win_disconnect(conn);
    printf("[term] Exited cleanly.\n");
    fflush(stdout);
    return 0;
}
