#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>

#define TEST_PASS(desc) printf("PASS: %s\n", desc)
#define TEST_FAIL(desc, msg) do { printf("FAIL: %s (%s, errno=%d)\n", desc, msg, errno); exit(1); } while(0)

static volatile sig_atomic_t got_sigpipe = 0;
static void handle_sigpipe(int sig) {
    (void) sig;
    got_sigpipe = 1;
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("=== Starting Stage 18B Step 3 AF_UNIX Socket Test Suite ===\n");

    /* 1. Protocol validation */
    printf("\n[1] Testing socket() domain and type validation...\n");
    int bad_s1 = socket(AF_INET, SOCK_STREAM, 0);
    if (bad_s1 >= 0 || (errno != EAFNOSUPPORT && errno != EINVAL)) {
        TEST_FAIL("AF_INET rejection", "expected -EAFNOSUPPORT");
    }
    int bad_s2 = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (bad_s2 >= 0 || (errno != EPROTONOSUPPORT && errno != EOPNOTSUPP)) {
        TEST_FAIL("SOCK_DGRAM rejection", "expected -EPROTONOSUPPORT");
    }
    int s_test = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s_test < 0) {
        TEST_FAIL("socket(AF_UNIX, SOCK_STREAM, 0)", "failed to create socket");
    }
    close(s_test);
    TEST_PASS("socket creation and parameter validation");

    /* 2. Bind validation */
    printf("\n[2] Testing bind() to tmpfs vs non-tmpfs and filesystem node creation...\n");
    int s_fat = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un_fat;
    memset(&un_fat, 0, sizeof(un_fat));
    un_fat.sun_family = AF_UNIX;
    strcpy(un_fat.sun_path, "/test_fat.sock");
    int err = bind(s_fat, (struct sockaddr*) &un_fat, sizeof(un_fat));
    if (err == 0 || (errno != EOPNOTSUPP && errno != EACCES)) {
        TEST_FAIL("bind to FAT non-tmpfs path", "expected -EOPNOTSUPP");
    }
    close(s_fat);

    /* Bind to /run (tmpfs) */
    unlink("/run/test.sock");
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) TEST_FAIL("server socket create", "socket() failed");

    struct sockaddr_un un_srv;
    memset(&un_srv, 0, sizeof(un_srv));
    un_srv.sun_family = AF_UNIX;
    strcpy(un_srv.sun_path, "/run/test.sock");

    if (bind(srv, (struct sockaddr*) &un_srv, sizeof(un_srv)) < 0) {
        TEST_FAIL("bind to /run/test.sock", "bind() failed");
    }

    /* Check S_IFSOCK node in tmpfs */
    struct stat st;
    if (stat("/run/test.sock", &st) < 0) {
        TEST_FAIL("stat(/run/test.sock)", "file not found");
    }
    if (!S_ISSOCK(st.st_mode)) {
        TEST_FAIL("stat S_IFSOCK bit", "mode is not S_IFSOCK");
    }

    /* Test EADDRINUSE on second bind */
    int s_dup = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(s_dup, (struct sockaddr*) &un_srv, sizeof(un_srv)) == 0 || errno != EADDRINUSE) {
        TEST_FAIL("duplicate bind EADDRINUSE", "expected EADDRINUSE");
    }
    close(s_dup);
    TEST_PASS("bind to tmpfs, S_IFSOCK node verification, and EADDRINUSE");

    /* 3. Connect error checking */
    printf("\n[3] Testing connect() errors (ENOENT and ECONNREFUSED)...\n");
    int cl_err = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un_nonexist;
    memset(&un_nonexist, 0, sizeof(un_nonexist));
    un_nonexist.sun_family = AF_UNIX;
    strcpy(un_nonexist.sun_path, "/run/nonexistent.sock");
    if (connect(cl_err, (struct sockaddr*) &un_nonexist, sizeof(un_nonexist)) == 0 || errno != ENOENT) {
        TEST_FAIL("connect to nonexistent socket", "expected ENOENT");
    }

    /* Connect before listen -> ECONNREFUSED */
    if (connect(cl_err, (struct sockaddr*) &un_srv, sizeof(un_srv)) == 0 || errno != ECONNREFUSED) {
        TEST_FAIL("connect to unlistened socket", "expected ECONNREFUSED");
    }
    close(cl_err);
    TEST_PASS("connect() ENOENT and ECONNREFUSED");

    /* 4. Listen & Accept */
    printf("\n[4] Testing listen(), non-blocking accept, and blocking accept...\n");
    if (listen(srv, 5) < 0) {
        TEST_FAIL("listen()", "listen failed");
    }

    /* Non-blocking accept on empty queue -> EAGAIN */
    int srv_flags = fcntl(srv, F_GETFL, 0);
    fcntl(srv, F_SETFL, srv_flags | O_NONBLOCK);
    struct sockaddr_un cl_addr;
    socklen_t cl_len = sizeof(cl_addr);
    int acc_nb = accept(srv, (struct sockaddr*) &cl_addr, &cl_len);
    if (acc_nb >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        TEST_FAIL("non-blocking accept on empty queue", "expected EAGAIN");
    }
    fcntl(srv, F_SETFL, srv_flags); /* restore blocking */

    /* Fork a client process to connect and test communication */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: client */
        usleep(50000); /* 50ms */
        int cl = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cl < 0) exit(10);
        if (connect(cl, (struct sockaddr*) &un_srv, sizeof(un_srv)) < 0) exit(11);

        /* Send hello message */
        const char msg[] = "HELLO_FROM_CLIENT";
        if (send(cl, msg, strlen(msg), 0) != (ssize_t)strlen(msg)) exit(12);

        /* Receive response */
        char resp[64];
        memset(resp, 0, sizeof(resp));
        int n = recv(cl, resp, sizeof(resp) - 1, 0);
        if (n <= 0 || strcmp(resp, "HELLO_FROM_SERVER") != 0) exit(13);

        close(cl);
        exit(0);
    }

    /* Parent: server accepts client */
    int conn = accept(srv, (struct sockaddr*) &cl_addr, &cl_len);
    if (conn < 0) {
        TEST_FAIL("blocking accept", "accept failed");
    }

    /* Read client message */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int nr = read(conn, buf, sizeof(buf) - 1);
    if (nr <= 0 || strcmp(buf, "HELLO_FROM_CLIENT") != 0) {
        TEST_FAIL("receive client message", "mismatch");
    }

    /* Send response */
    const char srv_msg[] = "HELLO_FROM_SERVER";
    if (write(conn, srv_msg, strlen(srv_msg)) != (int)strlen(srv_msg)) {
        TEST_FAIL("send server response", "write failed");
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        TEST_FAIL("client process exit status", "client reported failure");
    }
    close(conn);
    TEST_PASS("listen, blocking & non-blocking accept, and basic communication");

    /* 5. 64KB Ring Buffer & Heavy Data Stream */
    printf("\n[5] Testing 64KB ring buffer under heavy backpressure (128KB transfer)...\n");
    pid = fork();
    if (pid == 0) {
        int cl = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cl, (struct sockaddr*) &un_srv, sizeof(un_srv)) < 0) exit(20);

        /* Send 128KB in 4KB chunks */
        char chunk[4096];
        for (int i = 0; i < 4096; i++) chunk[i] = (char)(i & 0xFF);

        for (int k = 0; k < 32; k++) {
            int sent = 0;
            while (sent < 4096) {
                int s = send(cl, chunk + sent, 4096 - sent, 0);
                if (s <= 0) exit(21);
                sent += s;
            }
        }
        close(cl);
        exit(0);
    }

    conn = accept(srv, NULL, NULL);
    if (conn < 0) TEST_FAIL("accept for heavy transfer", "failed");

    /* Receive 128KB and verify checksum */
    uint32_t total_received = 0;
    char rx_buf[4096];
    int chunk_idx = 0;
    int corrupt = 0;

    while (1) {
        int r = read(conn, rx_buf, sizeof(rx_buf));
        if (r == 0) break; /* EOF */
        if (r < 0) {
            TEST_FAIL("read during 128KB stream", "error returned");
        }
        for (int i = 0; i < r; i++) {
            char expected = (char)((chunk_idx + i) & 0xFF);
            if (rx_buf[i] != expected) corrupt = 1;
        }
        chunk_idx = (chunk_idx + r) % 4096;
        total_received += r;
    }

    close(conn);
    waitpid(pid, &status, 0);

    if (total_received != 131072) {
        TEST_FAIL("128KB stream length", "byte count mismatch");
    }
    if (corrupt) {
        TEST_FAIL("128KB stream integrity", "data corruption detected");
    }
    TEST_PASS("64KB ring buffer with 128KB backpressure stream without corruption");

    /* 6. Shutdown, EOF, and SIGPIPE */
    printf("\n[6] Testing shutdown(), EOF, and SIGPIPE / MSG_NOSIGNAL...\n");
    signal(SIGPIPE, handle_sigpipe);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        TEST_FAIL("socketpair", "failed");
    }

    /* Shutdown write on sv[0] -> sv[1] sees EOF */
    if (shutdown(sv[0], SHUT_WR) < 0) {
        TEST_FAIL("shutdown SHUT_WR", "failed");
    }

    char eof_test[16];
    int n_eof = read(sv[1], eof_test, sizeof(eof_test));
    if (n_eof != 0) {
        TEST_FAIL("EOF on shutdown reader", "expected 0");
    }

    /* Close sv[1] and attempt to write on sv[0] -> EPIPE and SIGPIPE */
    close(sv[1]);
    got_sigpipe = 0;
    int wr_err = write(sv[0], "test", 4);
    if (wr_err >= 0 || errno != EPIPE) {
        TEST_FAIL("write to closed socket", "expected EPIPE");
    }
    if (!got_sigpipe) {
        TEST_FAIL("SIGPIPE generation", "handler not invoked");
    }

    /* Test MSG_NOSIGNAL suppression */
    got_sigpipe = 0;
    int wr_nosig = send(sv[0], "test", 4, MSG_NOSIGNAL);
    if (wr_nosig >= 0 || errno != EPIPE) {
        TEST_FAIL("send MSG_NOSIGNAL", "expected EPIPE");
    }
    if (got_sigpipe) {
        TEST_FAIL("MSG_NOSIGNAL suppression", "SIGPIPE was not suppressed");
    }
    close(sv[0]);
    TEST_PASS("shutdown(SHUT_WR), EOF, EPIPE, and MSG_NOSIGNAL");

    /* 7. socketpair bidirectional test */
    printf("\n[7] Testing socketpair() bidirectional communication across fork...\n");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        TEST_FAIL("socketpair()", "failed");
    }

    pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char b[32];
        memset(b, 0, sizeof(b));
        read(sv[1], b, sizeof(b) - 1);
        if (strcmp(b, "PING") != 0) exit(30);
        write(sv[1], "PONG", 4);
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    write(sv[0], "PING", 4);
    char pong[32];
    memset(pong, 0, sizeof(pong));
    read(sv[0], pong, sizeof(pong) - 1);
    if (strcmp(pong, "PONG") != 0) {
        TEST_FAIL("socketpair PONG mismatch", "received bad response");
    }
    close(sv[0]);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        TEST_FAIL("socketpair child exit status", "non-zero exit");
    }
    TEST_PASS("socketpair bidirectional ping-pong across fork");

    /* 8. Procfs validation (/proc/net/unix and /proc/self/fd) */
    printf("\n[8] Testing /proc/net/unix and /proc/self/fd...\n");
    int fd_proc = open("/proc/net/unix", O_RDONLY);
    if (fd_proc < 0) {
        TEST_FAIL("open /proc/net/unix", "failed to open");
    }
    char pbuf[1024];
    memset(pbuf, 0, sizeof(pbuf));
    int pr = read(fd_proc, pbuf, sizeof(pbuf) - 1);
    close(fd_proc);
    if (pr <= 0 || !strstr(pbuf, "/run/test.sock")) {
        TEST_FAIL("/proc/net/unix content", "/run/test.sock not listed");
    }

    /* Check /proc/self/fd for server socket */
    char path_fd[64];
    snprintf(path_fd, sizeof(path_fd), "/proc/self/fd/%d", srv);
    int fd_desc = open(path_fd, O_RDONLY);
    if (fd_desc >= 0) {
        char fdbuf[64];
        memset(fdbuf, 0, sizeof(fdbuf));
        read(fd_desc, fdbuf, sizeof(fdbuf) - 1);
        close(fd_desc);
        if (!strstr(fdbuf, "socket:[")) {
            TEST_FAIL("/proc/self/fd entry", "expected socket:[inode]");
        }
    }
    TEST_PASS("/proc/net/unix and /proc/self/fd verification");

    /* Cleanup */
    close(srv);
    unlink("/run/test.sock");

    printf("\n>>> ALL STAGE 18B STEP 3 SOCKET TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
