#include "libc.h"

static void print_str(const char* s) {
    write(1, s, strlen(s));
}

static void assert_true(int condition, const char* msg) {
    if (!condition) {
        print_str("FAIL: ");
        print_str(msg);
        print_str("\n");
        exit(1);
    }
    print_str("PASS: ");
    print_str(msg);
    print_str("\n");
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    print_str("=== Running TTY & Device Nodes Tests ===\n");

    /* Test 1: isatty on stdin */
    assert_true(isatty(0) == 1, "isatty(0) is true for /dev/console");

    /* Test 2: isatty on regular file */
    int fd_file = open("/tmp/tty_test.txt", O_CREAT | O_RDWR, 0644);
    assert_true(fd_file >= 0, "open temporary file");
    assert_true(isatty(fd_file) == 0, "isatty on file is false");
    close(fd_file);

    /* Test 3: /dev/null */
    int fd_null = open("/dev/null", O_RDWR, 0);
    assert_true(fd_null >= 0, "open /dev/null");
    assert_true(isatty(fd_null) == 0, "isatty(/dev/null) is false");
    char null_write_buf[] = "hello null";
    int nw = write(fd_null, null_write_buf, sizeof(null_write_buf));
    assert_true(nw == (int)sizeof(null_write_buf), "write to /dev/null swallows bytes");
    char null_read_buf[16];
    int nr = read(fd_null, null_read_buf, sizeof(null_read_buf));
    assert_true(nr == 0, "read from /dev/null returns 0 (EOF)");
    close(fd_null);

    /* Test 4: /dev/zero */
    int fd_zero = open("/dev/zero", O_RDONLY, 0);
    assert_true(fd_zero >= 0, "open /dev/zero");
    assert_true(isatty(fd_zero) == 0, "isatty(/dev/zero) is false");
    char zero_buf[32];
    for (int i = 0; i < 32; i++) zero_buf[i] = 0x55;
    int zr = read(fd_zero, zero_buf, 32);
    assert_true(zr == 32, "read from /dev/zero returns requested bytes");
    int all_zeros = 1;
    for (int i = 0; i < 32; i++) {
        if (zero_buf[i] != 0) all_zeros = 0;
    }
    assert_true(all_zeros == 1, "read from /dev/zero produces all zeros");
    close(fd_zero);

    /* Test 5: window size via ioctl TIOCGWINSZ */
    struct winsize ws;
    int ret_ws = ioctl(0, TIOCGWINSZ, &ws);
    assert_true(ret_ws == 0, "ioctl(0, TIOCGWINSZ) succeeds");
    assert_true(ws.ws_row == 25 && ws.ws_col == 80, "winsize is 80x25");

    /* Test 6: tcgetattr and flags */
    struct termios orig_t;
    int ret_ga = tcgetattr(0, &orig_t);
    assert_true(ret_ga == 0, "tcgetattr(0) succeeds");
    assert_true((orig_t.c_lflag & ICANON) != 0, "default termios has ICANON");
    assert_true((orig_t.c_lflag & ECHO) != 0, "default termios has ECHO");
    assert_true((orig_t.c_lflag & ISIG) != 0, "default termios has ISIG");

    /* Test 7: process group via TIOCGPGRP and TIOCSPGRP */
    int pgrp = 0;
    int ret_gp = ioctl(0, TIOCGPGRP, &pgrp);
    assert_true(ret_gp == 0, "ioctl(0, TIOCGPGRP) succeeds");
    int new_pgrp = pgrp;
    int ret_sp = ioctl(0, TIOCSPGRP, &new_pgrp);
    assert_true(ret_sp == 0, "ioctl(0, TIOCSPGRP) succeeds");

    /* Test 8: raw mode non-blocking with VMIN=0 */
    struct termios raw_t = orig_t;
    raw_t.c_lflag &= ~(ICANON | ECHO);
    raw_t.c_cc[VMIN] = 0;
    raw_t.c_cc[VTIME] = 0;
    int ret_sa = tcsetattr(0, TCSANOW, &raw_t);
    assert_true(ret_sa == 0, "tcsetattr(raw mode, VMIN=0) succeeds");

    struct termios check_t;
    tcgetattr(0, &check_t);
    assert_true((check_t.c_lflag & ICANON) == 0, "ICANON is disabled in raw mode");

    char nonblock_buf[8];
    int nbr = read(0, nonblock_buf, sizeof(nonblock_buf));
    assert_true(nbr == 0, "non-blocking read with VMIN=0 returns 0 immediately");

    /* Restore canonical mode */
    ret_sa = tcsetattr(0, TCSANOW, &orig_t);
    assert_true(ret_sa == 0, "restore canonical termios succeeds");

    print_str("TTYTEST_PASSED\n");
    return 0;
}
