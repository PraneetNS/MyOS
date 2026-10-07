#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;      /* input modes */
    tcflag_t c_oflag;      /* output modes */
    tcflag_t c_cflag;      /* control modes */
    tcflag_t c_lflag;      /* local modes */
    cc_t     c_line;       /* line discipline */
    cc_t     c_cc[NCCS];   /* control characters */
    speed_t  c_ispeed;     /* input baud rate */
    speed_t  c_ospeed;     /* output baud rate */
};

/* c_iflag bits */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

/* c_oflag bits */
#define OPOST   0000001
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040

/* c_cflag bits */
#define CSIZE   0000060
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000

/* c_lflag bits */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000

/* c_cc indices */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSWTC   7
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* ioctl requests */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

#define TTY_CANON_BUF_SIZE 1024
#define TTY_QUEUE_SIZE     2048

typedef struct tty {
    struct termios termios;
    struct winsize winsize;
    int fg_pgid;

    /* Canonical mode line editing buffer */
    char canon_buf[TTY_CANON_BUF_SIZE];
    int canon_len;

    /* Completed lines ready to be read in canonical mode */
    char line_queue[TTY_QUEUE_SIZE];
    int line_head;
    int line_tail;
    int line_count;

    /* Raw mode input queue */
    char raw_queue[TTY_QUEUE_SIZE];
    int raw_head;
    int raw_tail;
    int raw_count;

    int eof_pending;
    int read_wait;
} tty_t;

extern tty_t* global_tty;

void tty_init(void);
tty_t* tty_get_global(void);
int tty_is_canonical(tty_t* tty);
void tty_input_char(tty_t* tty, char c);
void tty_input_string(tty_t* tty, const char* s);
int tty_read(tty_t* tty, char* buf, uint32_t count);
int tty_write(tty_t* tty, const char* buf, uint32_t count);
int tty_ioctl(tty_t* tty, unsigned long req, void* argp);

#endif
