#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        g_tests_failed++; \
    } \
} while (0)

static void test_fcntl_flags(void) {
    printf("--- Test 1: fcntl F_GETFL / F_SETFL ---\n");
    int fds[2];
    if (pipe(fds) < 0) {
        printf("  [FAIL] pipe failed\n");
        g_tests_failed++;
        return;
    }

    int flags = fcntl(fds[0], F_GETFL);
    TEST_ASSERT((flags & O_NONBLOCK) == 0, "pipe read end initially blocking");

    int ret = fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    TEST_ASSERT(ret == 0, "fcntl F_SETFL O_NONBLOCK succeeded");

    int new_flags = fcntl(fds[0], F_GETFL);
    TEST_ASSERT((new_flags & O_NONBLOCK) != 0, "fcntl F_GETFL reflects O_NONBLOCK");

    close(fds[0]);
    close(fds[1]);
}

static void test_nonblock_pipe_read(void) {
    printf("--- Test 2: non-blocking pipe read ---\n");
    int fds[2];
    if (pipe(fds) < 0) return;

    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    char buf[16];
    errno = 0;
    int n = read(fds[0], buf, sizeof(buf));
    TEST_ASSERT(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "nonblock read on empty pipe returns -1 with EAGAIN");

    write(fds[1], "test", 4);
    n = read(fds[0], buf, sizeof(buf));
    TEST_ASSERT(n == 4 && memcmp(buf, "test", 4) == 0, "nonblock read with data succeeds");

    n = read(fds[0], buf, sizeof(buf));
    TEST_ASSERT(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "subsequent nonblock read returns EAGAIN");

    close(fds[0]);
    close(fds[1]);
}

static void test_poll_timeout(void) {
    printf("--- Test 3: poll timeout ---\n");
    int fds[2];
    if (pipe(fds) < 0) return;

    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 100); /* 100ms timeout */
    TEST_ASSERT(ret == 0, "poll on empty pipe returns 0 (timeout)");
    TEST_ASSERT(pfd.revents == 0, "revents is 0 on timeout");

    close(fds[0]);
    close(fds[1]);
}

static void test_poll_ready_and_hup(void) {
    printf("--- Test 4: poll ready and POLLHUP ---\n");
    int fds[2];
    if (pipe(fds) < 0) return;

    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    write(fds[1], "A", 1);
    int ret = poll(&pfd, 1, 100);
    TEST_ASSERT(ret == 1, "poll with data returns 1");
    TEST_ASSERT((pfd.revents & POLLIN) != 0, "poll revents has POLLIN");

    char c;
    read(fds[0], &c, 1);

    close(fds[1]); /* close writer */
    pfd.revents = 0;
    ret = poll(&pfd, 1, 100);
    TEST_ASSERT(ret == 1, "poll on closed pipe writer returns 1");
    TEST_ASSERT((pfd.revents & POLLHUP) != 0, "poll revents has POLLHUP");

    close(fds[0]);
}

static void test_select(void) {
    printf("--- Test 5: select functionality ---\n");
    int fds[2];
    if (pipe(fds) < 0) return;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50000; /* 50ms */

    int ret = select(fds[0] + 1, &rfds, NULL, NULL, &tv);
    TEST_ASSERT(ret == 0, "select on empty pipe times out");
    TEST_ASSERT(!FD_ISSET(fds[0], &rfds), "fd not set after timeout");

    write(fds[1], "ok", 2);
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 50000;

    ret = select(fds[0] + 1, &rfds, NULL, NULL, &tv);
    TEST_ASSERT(ret == 1, "select with data returns 1");
    TEST_ASSERT(FD_ISSET(fds[0], &rfds), "fd is set when data available");

    char buf[4];
    read(fds[0], buf, 2);

    close(fds[0]);
    close(fds[1]);
}

static void sigalrm_handler(int sig) {
    (void) sig;
}

static void test_poll_eintr(void) {
    printf("--- Test 6: poll EINTR on signal ---\n");
    int fds[2];
    if (pipe(fds) < 0) return;

    struct sigaction sa;
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);

    alarm(1); /* 1 second timer */

    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    errno = 0;
    int ret = poll(&pfd, 1, 3000); /* wait up to 3 seconds */
    TEST_ASSERT(ret == -1 && errno == EINTR, "poll interrupted by signal returns -1 with EINTR");

    alarm(0);
    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    printf("=== Starting polltest ===\n");
    test_fcntl_flags();
    test_nonblock_pipe_read();
    test_poll_timeout();
    test_poll_ready_and_hup();
    test_select();
    test_poll_eintr();

    printf("\n=== polltest results: %d passed, %d failed ===\n", g_tests_passed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
