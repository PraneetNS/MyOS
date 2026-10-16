#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <pty.h>

#define TEST_PASS(desc) printf("PASS: %s\n", desc)
#define TEST_FAIL(desc, msg) do { printf("FAIL: %s (%s, errno=%d)\n", desc, msg, errno); exit(1); } while(0)

static volatile sig_atomic_t got_sigwinch = 0;
static void handle_sigwinch(int sig) {
    (void) sig;
    got_sigwinch = 1;
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("=== Starting Stage 18B Step 4 PTY & TTY Test Suite ===\n");

    /* 1. Basic PTY allocation via /dev/ptmx and ptsname */
    printf("\n[1] Testing posix_openpt, grantpt, unlockpt, and ptsname_r...\n");
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        TEST_FAIL("posix_openpt", "failed to open /dev/ptmx");
    }

    if (grantpt(master) < 0) {
        TEST_FAIL("grantpt", "failed");
    }
    if (unlockpt(master) < 0) {
        TEST_FAIL("unlockpt", "failed");
    }

    char slave_path[64];
    if (ptsname_r(master, slave_path, sizeof(slave_path)) != 0) {
        TEST_FAIL("ptsname_r", "failed to get pts name");
    }
    printf("Allocated PTY: master fd=%d, slave=%s\n", master, slave_path);

    /* Verify slave exists in /dev/pts and is a character device */
    struct stat st;
    if (stat(slave_path, &st) < 0) {
        TEST_FAIL("stat slave device", "file not found in /dev/pts");
    }
    if (!S_ISCHR(st.st_mode)) {
        TEST_FAIL("slave is character device", "st_mode is not S_IFCHR");
    }
    TEST_PASS("PTY allocation, grantpt/unlockpt, and slave device verification");

    /* 2. Open slave and test line discipline canonical mode */
    printf("\n[2] Testing open(slave) and canonical line discipline...\n");
    int slave = open(slave_path, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        TEST_FAIL("open slave", "failed");
    }

    /* Master writes input with carriage return \r */
    const char in_str[] = "hello\r";
    if (write(master, in_str, strlen(in_str)) != (int)strlen(in_str)) {
        TEST_FAIL("write to master", "failed");
    }

    /* Slave reads: ICRNL translates \r -> \n */
    char slave_buf[32];
    memset(slave_buf, 0, sizeof(slave_buf));
    int nr = read(slave, slave_buf, sizeof(slave_buf) - 1);
    if (nr <= 0) {
        TEST_FAIL("slave read", "read returned <= 0");
    }
    if (strcmp(slave_buf, "hello\n") != 0) {
        printf("slave received: '%s'\n", slave_buf);
        TEST_FAIL("canonical line editing", "expected 'hello\\n'");
    }

    /* Master reads echo: ECHO + ONLCR produces 'hello\r\n' */
    char echo_buf[32];
    memset(echo_buf, 0, sizeof(echo_buf));
    int ne = read(master, echo_buf, sizeof(echo_buf) - 1);
    if (ne <= 0) {
        TEST_FAIL("master read echo", "read returned <= 0");
    }
    if (strstr(echo_buf, "hello") == NULL) {
        printf("echo received: '%s'\n", echo_buf);
        TEST_FAIL("echo verification", "did not find 'hello' in echo buffer");
    }
    TEST_PASS("canonical mode, ICRNL, and echo back to master");

    /* 3. Window size change and SIGWINCH delivery */
    printf("\n[3] Testing TIOCSWINSZ / TIOCGWINSZ and SIGWINCH...\n");
    signal(SIGWINCH, handle_sigwinch);
    signal(SIGHUP, SIG_IGN);

    /* Set controlling terminal and foreground process group */
    pid_t my_pid = getpid();
    ioctl(slave, TIOCSCTTY, 0);
    ioctl(slave, TIOCSPGRP, &my_pid);

    got_sigwinch = 0;
    struct winsize ws;
    ws.ws_row = 40;
    ws.ws_col = 100;
    ws.ws_xpixel = 800;
    ws.ws_ypixel = 600;

    if (ioctl(master, TIOCSWINSZ, &ws) < 0) {
        TEST_FAIL("ioctl(master, TIOCSWINSZ)", "failed");
    }

    /* Verify SIGWINCH received */
    if (!got_sigwinch) {
        TEST_FAIL("SIGWINCH delivery", "handler was not called");
    }

    /* Verify slave sees new window dimensions */
    struct winsize slave_ws;
    memset(&slave_ws, 0, sizeof(slave_ws));
    if (ioctl(slave, TIOCGWINSZ, &slave_ws) < 0) {
        TEST_FAIL("ioctl(slave, TIOCGWINSZ)", "failed");
    }
    if (slave_ws.ws_row != 40 || slave_ws.ws_col != 100) {
        TEST_FAIL("TIOCGWINSZ values", "row/col mismatch");
    }
    TEST_PASS("TIOCSWINSZ, SIGWINCH signal delivery, and TIOCGWINSZ");

    ioctl(slave, TIOCNOTTY, 0);
    close(slave);
    close(master);

    /* 4. openpty helper function */
    printf("\n[4] Testing openpty() helper...\n");
    int pty_m = -1, pty_s = -1;
    char pty_name[64];
    ws.ws_row = 30;
    ws.ws_col = 90;
    if (openpty(&pty_m, &pty_s, pty_name, NULL, &ws) < 0) {
        TEST_FAIL("openpty()", "failed");
    }
    if (pty_m < 0 || pty_s < 0) {
        TEST_FAIL("openpty() descriptors", "invalid fds");
    }
    memset(&slave_ws, 0, sizeof(slave_ws));
    if (ioctl(pty_s, TIOCGWINSZ, &slave_ws) < 0 || slave_ws.ws_row != 30 || slave_ws.ws_col != 90) {
        TEST_FAIL("openpty() winsize propagation", "mismatch");
    }
    close(pty_m);
    close(pty_s);
    TEST_PASS("openpty() initialization and parameters");

    /* 5. forkpty() session and communication across fork */
    printf("\n[5] Testing forkpty() child session and bidirectional stream...\n");
    int fmaster = -1;
    pid_t child = forkpty(&fmaster, NULL, NULL, NULL);
    if (child < 0) {
        TEST_FAIL("forkpty()", "failed");
    }

    if (child == 0) {
        /* Child: in new session with slave as ctty on stdin/stdout/stderr */
        if (!isatty(0) || !isatty(1) || !isatty(2)) {
            exit(50);
        }

        /* Read request line from master via stdin */
        char in_line[64];
        memset(in_line, 0, sizeof(in_line));
        if (fgets(in_line, sizeof(in_line), stdin) == NULL) {
            exit(51);
        }

        /* Verify message and send reply via stdout */
        if (strstr(in_line, "PING_FROM_MASTER") == NULL) {
            exit(52);
        }

        printf("PONG_FROM_SLAVE\n");
        fflush(stdout);
        exit(0);
    }

    /* Parent: write to child via master fd */
    const char ping_msg[] = "PING_FROM_MASTER\n";
    if (write(fmaster, ping_msg, strlen(ping_msg)) != (int)strlen(ping_msg)) {
        TEST_FAIL("write to child via fmaster", "failed");
    }

    /* Read child response */
    char child_resp[128];
    memset(child_resp, 0, sizeof(child_resp));
    int total_read = 0;
    while (total_read < (int)sizeof(child_resp) - 1) {
        int r = read(fmaster, child_resp + total_read, sizeof(child_resp) - 1 - total_read);
        if (r <= 0) break;
        total_read += r;
        if (strstr(child_resp, "PONG_FROM_SLAVE") != NULL) break;
    }

    if (strstr(child_resp, "PONG_FROM_SLAVE") == NULL) {
        printf("Parent received: '%s'\n", child_resp);
        TEST_FAIL("forkpty communication", "did not receive PONG_FROM_SLAVE");
    }

    int cstatus = 0;
    waitpid(child, &cstatus, 0);
    if (!WIFEXITED(cstatus) || WEXITSTATUS(cstatus) != 0) {
        printf("child exit status: %d\n", WEXITSTATUS(cstatus));
        TEST_FAIL("child process exit code", "child reported failure");
    }
    close(fmaster);
    TEST_PASS("forkpty() session, isatty, and bidirectional IPC");

    /* 6. Virtual Consoles in /dev */
    printf("\n[6] Testing /dev/tty0, /dev/tty1, /dev/tty2, /dev/tty3 existence...\n");
    int fd_t0 = open("/dev/tty0", O_RDWR);
    if (fd_t0 < 0) TEST_FAIL("open /dev/tty0", "failed");
    close(fd_t0);

    int fd_t1 = open("/dev/tty1", O_RDWR);
    if (fd_t1 < 0) TEST_FAIL("open /dev/tty1", "failed");
    close(fd_t1);

    int fd_t2 = open("/dev/tty2", O_RDWR);
    if (fd_t2 < 0) TEST_FAIL("open /dev/tty2", "failed");
    close(fd_t2);

    int fd_t3 = open("/dev/tty3", O_RDWR);
    if (fd_t3 < 0) TEST_FAIL("open /dev/tty3", "failed");
    close(fd_t3);
    TEST_PASS("/dev/tty0-3 virtual consoles available");

    printf("\n>>> ALL STAGE 18B STEP 4 PTY TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
