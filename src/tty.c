#include "tty.h"
#include "vga.h"
#include "serial.h"
#include "scheduler.h"
#include "errno.h"
#include "uaccess.h"

static tty_t main_tty;
tty_t* global_tty = &main_tty;

void tty_init(void) {
    tty_t* tty = &main_tty;
    for (size_t i = 0; i < sizeof(tty_t); i++) {
        ((uint8_t*)tty)[i] = 0;
    }

    tty->termios.c_iflag = ICRNL | IXON;
    tty->termios.c_oflag = OPOST | ONLCR;
    tty->termios.c_cflag = CS8 | CREAD;
    tty->termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;

    tty->termios.c_cc[VINTR] = 3;     /* Ctrl+C */
    tty->termios.c_cc[VQUIT] = 28;    /* Ctrl+\ */
    tty->termios.c_cc[VERASE] = 0x7F; /* Backspace / DEL */
    tty->termios.c_cc[VKILL] = 21;    /* Ctrl+U */
    tty->termios.c_cc[VEOF] = 4;      /* Ctrl+D */
    tty->termios.c_cc[VTIME] = 0;
    tty->termios.c_cc[VMIN] = 1;
    tty->termios.c_cc[VSUSP] = 26;    /* Ctrl+Z */
    tty->termios.c_cc[VWERASE] = 23;  /* Ctrl+W */

    tty->winsize.ws_row = 25;
    tty->winsize.ws_col = 80;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;

    tty->fg_pgid = 1;
    tty->canon_len = 0;
    tty->line_head = 0;
    tty->line_tail = 0;
    tty->line_count = 0;
    tty->raw_head = 0;
    tty->raw_tail = 0;
    tty->raw_count = 0;
    tty->eof_pending = 0;
    tty->read_wait = 0;
}

tty_t* tty_get_global(void) {
    return global_tty;
}

int tty_is_canonical(tty_t* tty) {
    if (!tty) return 1;
    return (tty->termios.c_lflag & ICANON) != 0;
}

static void tty_echo_char(tty_t* tty, char c) {
    if (!(tty->termios.c_lflag & ECHO)) return;
    if (c == '\n') {
        terminal_putchar('\n');
        serial_putc('\n');
    } else {
        terminal_putchar(c);
        serial_putc(c);
    }
}

static void tty_echo_erase(tty_t* tty) {
    if (!(tty->termios.c_lflag & ECHO)) return;
    terminal_putchar('\b');
    serial_putc('\b');
    serial_putc(' ');
    serial_putc('\b');
}

void tty_input_char(tty_t* tty, char c) {
    if (!tty) return;

    /* Input mode conversions */
    if ((tty->termios.c_iflag & ICRNL) && c == '\r') {
        c = '\n';
    } else if ((tty->termios.c_iflag & INLCR) && c == '\n') {
        c = '\r';
    }
    if ((tty->termios.c_iflag & IGNCR) && c == '\r') {
        return;
    }

    if (tty->termios.c_lflag & ICANON) {
        /* Canonical mode line editing */
        if (c == '\n') {
            tty_echo_char(tty, '\n');
            if (tty->canon_len < TTY_CANON_BUF_SIZE - 1) {
                tty->canon_buf[tty->canon_len++] = '\n';
            }
            /* Transfer canon_buf to line_queue */
            for (int i = 0; i < tty->canon_len; i++) {
                if (tty->line_count < TTY_QUEUE_SIZE) {
                    tty->line_queue[tty->line_head] = tty->canon_buf[i];
                    tty->line_head = (tty->line_head + 1) % TTY_QUEUE_SIZE;
                    tty->line_count++;
                }
            }
            tty->canon_len = 0;
            scheduler_wake_channel(&tty->read_wait);
        } else if (c == tty->termios.c_cc[VEOF]) { /* Ctrl+D */
            if (tty->canon_len == 0) {
                tty->eof_pending = 1;
                scheduler_wake_channel(&tty->read_wait);
            } else {
                /* Flush current line without trailing newline */
                for (int i = 0; i < tty->canon_len; i++) {
                    if (tty->line_count < TTY_QUEUE_SIZE) {
                        tty->line_queue[tty->line_head] = tty->canon_buf[i];
                        tty->line_head = (tty->line_head + 1) % TTY_QUEUE_SIZE;
                        tty->line_count++;
                    }
                }
                tty->canon_len = 0;
                scheduler_wake_channel(&tty->read_wait);
            }
        } else if (c == tty->termios.c_cc[VERASE] || c == '\b' || c == 0x7F) {
            if (tty->canon_len > 0) {
                tty->canon_len--;
                tty_echo_erase(tty);
            }
        } else if (c == tty->termios.c_cc[VKILL]) { /* Ctrl+U */
            while (tty->canon_len > 0) {
                tty->canon_len--;
                tty_echo_erase(tty);
            }
        } else if (c == tty->termios.c_cc[VWERASE]) { /* Ctrl+W */
            while (tty->canon_len > 0 && tty->canon_buf[tty->canon_len - 1] == ' ') {
                tty->canon_len--;
                tty_echo_erase(tty);
            }
            while (tty->canon_len > 0 && tty->canon_buf[tty->canon_len - 1] != ' ') {
                tty->canon_len--;
                tty_echo_erase(tty);
            }
        } else if ((unsigned char)c >= 32 || c == '\t') {
            if (tty->canon_len < TTY_CANON_BUF_SIZE - 2) {
                tty->canon_buf[tty->canon_len++] = c;
                tty_echo_char(tty, c);
            }
        }
    } else {
        /* Raw / non-canonical mode */
        if (tty->raw_count < TTY_QUEUE_SIZE) {
            tty->raw_queue[tty->raw_head] = c;
            tty->raw_head = (tty->raw_head + 1) % TTY_QUEUE_SIZE;
            tty->raw_count++;
        }
        tty_echo_char(tty, c);
        scheduler_wake_channel(&tty->read_wait);
    }
}

void tty_input_string(tty_t* tty, const char* s) {
    if (!tty || !s) return;
    while (*s) {
        tty_input_char(tty, *s++);
    }
}

int tty_read(tty_t* tty, char* buf, uint32_t count) {
    if (!tty || !buf || count == 0) return 0;

    if (tty->termios.c_lflag & ICANON) {
        /* Canonical mode: wait until line available or EOF */
        while (tty->line_count == 0 && !tty->eof_pending) {
            scheduler_wait_channel(&tty->read_wait);
        }

        if (tty->line_count == 0 && tty->eof_pending) {
            tty->eof_pending = 0;
            return 0;
        }

        uint32_t n = 0;
        while (n < count && tty->line_count > 0) {
            char ch = tty->line_queue[tty->line_tail];
            tty->line_tail = (tty->line_tail + 1) % TTY_QUEUE_SIZE;
            tty->line_count--;
            buf[n++] = ch;
            if (ch == '\n') break;
        }
        return (int) n;
    } else {
        /* Raw mode */
        uint8_t vmin = tty->termios.c_cc[VMIN];
        if (vmin > 0) {
            uint32_t needed = (vmin < count) ? vmin : count;
            while (tty->raw_count < (int) needed) {
                scheduler_wait_channel(&tty->read_wait);
            }
        } else {
            /* Non-blocking when VMIN == 0 */
            if (tty->raw_count == 0) return 0;
        }

        uint32_t n = 0;
        while (n < count && tty->raw_count > 0) {
            char ch = tty->raw_queue[tty->raw_tail];
            tty->raw_tail = (tty->raw_tail + 1) % TTY_QUEUE_SIZE;
            tty->raw_count--;
            buf[n++] = ch;
        }
        return (int) n;
    }
}

int tty_write(tty_t* tty, const char* buf, uint32_t count) {
    (void) tty;
    if (!buf || count == 0) return 0;
    for (uint32_t i = 0; i < count; i++) {
        char c = buf[i];
        terminal_putchar(c);
        serial_putc(c);
    }
    return (int) count;
}

int tty_ioctl(tty_t* tty, unsigned long req, void* argp) {
    if (!tty) return -ENOTTY;

    switch (req) {
        case TCGETS: {
            if (!argp) return -EFAULT;
            if (copy_to_user(argp, &tty->termios, sizeof(struct termios)) != 0)
                return -EFAULT;
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            if (!argp) return -EFAULT;
            struct termios new_t;
            if (copy_from_user(&new_t, argp, sizeof(struct termios)) != 0)
                return -EFAULT;
            tty->termios = new_t;
            if (req == TCSETSF) {
                tty->raw_head = tty->raw_tail = tty->raw_count = 0;
                tty->line_head = tty->line_tail = tty->line_count = 0;
                tty->canon_len = 0;
            }
            return 0;
        }
        case TIOCGWINSZ: {
            if (!argp) return -EFAULT;
            if (copy_to_user(argp, &tty->winsize, sizeof(struct winsize)) != 0)
                return -EFAULT;
            return 0;
        }
        case TIOCSWINSZ: {
            if (!argp) return -EFAULT;
            struct winsize new_ws;
            if (copy_from_user(&new_ws, argp, sizeof(struct winsize)) != 0)
                return -EFAULT;
            tty->winsize = new_ws;
            return 0;
        }
        case TIOCGPGRP: {
            if (!argp) return -EFAULT;
            int pgid = tty->fg_pgid;
            if (copy_to_user(argp, &pgid, sizeof(int)) != 0)
                return -EFAULT;
            return 0;
        }
        case TIOCSPGRP: {
            if (!argp) return -EFAULT;
            int pgid;
            if (copy_from_user(&pgid, argp, sizeof(int)) != 0)
                return -EFAULT;
            tty->fg_pgid = pgid;
            return 0;
        }
        default:
            return -EINVAL;
    }
}
