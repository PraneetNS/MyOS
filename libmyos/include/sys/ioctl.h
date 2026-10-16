#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCNOTTY   0x5422
#define TIOCGPTN    0x80045430
#define TIOCSPTLCK  0x40045431

int ioctl(int fd, unsigned long request, ...);

#endif
