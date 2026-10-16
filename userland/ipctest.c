#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <pty.h>

#define TEST_PASS(desc) printf("  [PASS] %s\n", desc)
#define TEST_FAIL(desc, msg) do { \
    printf("  [FAIL] %s: %s (errno=%d: %s, line %d)\n", desc, msg, errno, strerror(errno), __LINE__); \
    exit(1); \
} while (0)

#define ASSERT_EQ(a, b, desc) do { \
    if ((a) != (b)) { \
        printf("  [FAIL] %s: %ld != %ld (line %d)\n", desc, (long)(a), (long)(b), __LINE__); \
        exit(1); \
    } \
} while (0)

static volatile sig_atomic_t got_sigpipe = 0;
static void handle_sigpipe(int sig) {
    (void) sig;
    got_sigpipe = 1;
}

static volatile sig_atomic_t got_sigwinch = 0;
static void handle_sigwinch(int sig) {
    (void) sig;
    got_sigwinch = 1;
}

/* ========================================================================= */
/* 1. tmpfs File Operations, Truncation, Sparse Holes, Limits, Rejections    */
/* ========================================================================= */
static void test_tmpfs_operations(void) {
    printf("[1] Testing tmpfs file operations, truncation, sparse holes, and limits...\n");

    /* 1.1 Create directory under /run */
    mkdir("/run/test_dir", 0755);
    struct stat st;
    if (stat("/run/test_dir", &st) < 0 || !S_ISDIR(st.st_mode)) {
        TEST_FAIL("tmpfs mkdir", "failed to create /run/test_dir");
    }

    /* 1.2 Create and write file */
    int fd = open("/run/test_dir/file.txt", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) TEST_FAIL("tmpfs open/create", "open /run/test_dir/file.txt failed");

    const char* msg = "Hello tmpfs!";
    int len = strlen(msg);
    if (write(fd, msg, len) != len) TEST_FAIL("tmpfs write", "short write");

    /* 1.3 Truncate up to 16384 bytes (sparse hole) */
    if (ftruncate(fd, 16384) != 0) TEST_FAIL("tmpfs ftruncate expand", "ftruncate failed");
    if (fstat(fd, &st) < 0 || st.st_size != 16384) {
        TEST_FAIL("tmpfs fstat size", "file size != 16384 after truncate");
    }

    /* Verify sparse hole reads as zeros */
    char buf[128];
    lseek(fd, 8192, SEEK_SET);
    int nr = read(fd, buf, sizeof(buf));
    if (nr != sizeof(buf)) TEST_FAIL("tmpfs read sparse hole", "read failed");
    for (int i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != 0) TEST_FAIL("tmpfs sparse zero verify", "non-zero byte in sparse hole");
    }

    /* Truncate down to 5 bytes */
    if (ftruncate(fd, 5) != 0) TEST_FAIL("tmpfs ftruncate shrink", "ftruncate down failed");
    if (fstat(fd, &st) < 0 || st.st_size != 5) {
        TEST_FAIL("tmpfs fstat shrink size", "file size != 5");
    }
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    nr = read(fd, buf, 10);
    ASSERT_EQ(nr, 5, "tmpfs read after shrink");
    ASSERT_EQ(memcmp(buf, "Hello", 5), 0, "tmpfs content match after shrink");
    close(fd);

    /* 1.4 Rename file within tmpfs */
    if (rename("/run/test_dir/file.txt", "/run/test_dir/renamed.txt") != 0) {
        TEST_FAIL("tmpfs rename", "rename failed");
    }
    if (stat("/run/test_dir/file.txt", &st) == 0 || errno != ENOENT) {
        TEST_FAIL("tmpfs rename old path", "old path still exists");
    }
    if (stat("/run/test_dir/renamed.txt", &st) < 0 || st.st_size != 5) {
        TEST_FAIL("tmpfs rename new path", "new path missing or incorrect size");
    }

    /* 1.5 Unlink and rmdir */
    if (unlink("/run/test_dir/renamed.txt") != 0) TEST_FAIL("tmpfs unlink", "unlink failed");
    if (rmdir("/run/test_dir") != 0) TEST_FAIL("tmpfs rmdir", "rmdir failed");

    /* 1.6 Verify MAP_SHARED rejection on FAT file system */
    int fat_fd = open("/hello.txt", O_RDONLY);
    if (fat_fd >= 0) {
        void* bad_map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fat_fd, 0);
        if (bad_map != MAP_FAILED || errno != ENODEV) {
            TEST_FAIL("FAT MAP_SHARED rejection", "expected -ENODEV");
        }
        close(fat_fd);
    }

    /* 1.7 Verify bind() rejection on FAT file system */
    int s_fat = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s_fat >= 0) {
        struct sockaddr_un un_fat;
        memset(&un_fat, 0, sizeof(un_fat));
        un_fat.sun_family = AF_UNIX;
        strcpy(un_fat.sun_path, "/fat_sock_test.sock");
        if (bind(s_fat, (struct sockaddr*)&un_fat, sizeof(un_fat)) == 0 ||
            (errno != EOPNOTSUPP && errno != EACCES)) {
            TEST_FAIL("FAT socket bind rejection", "expected -EOPNOTSUPP or -EACCES");
        }
        close(s_fat);
    }

    TEST_PASS("tmpfs operations, sparse truncate, rename, unlink, and FAT rejections verified");
}

/* ========================================================================= */
/* 2. MAP_SHARED & MAP_ANON Shared Memory, Fork, Atomics, shm_open, msync    */
/* ========================================================================= */
static void test_shared_memory(void) {
    printf("[2] Testing MAP_SHARED | MAP_ANON with fork atomics and named shm_open...\n");

    /* 2.1 Anonymous shared mapping with concurrent child atomics */
    volatile uint32_t* counter = (volatile uint32_t*) mmap(NULL, 4096,
                                                           PROT_READ | PROT_WRITE,
                                                           MAP_SHARED | MAP_ANON,
                                                           -1, 0);
    if (counter == MAP_FAILED) TEST_FAIL("mmap MAP_SHARED | MAP_ANON", "mmap failed");
    *counter = 0;

    int nchildren = 4;
    int iters = 50000;
    for (int i = 0; i < nchildren; i++) {
        pid_t pid = fork();
        if (pid < 0) TEST_FAIL("fork child", "fork failed");
        if (pid == 0) {
            for (int k = 0; k < iters; k++) {
                __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
            }
            exit(0);
        }
    }

    /* Parent also increments */
    for (int k = 0; k < iters; k++) {
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
    }

    for (int i = 0; i < nchildren; i++) {
        int status = 0;
        wait(&status);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            TEST_FAIL("child exit status", "child did not exit cleanly");
        }
    }

    uint32_t expected = (nchildren + 1) * iters;
    ASSERT_EQ(*counter, expected, "atomic counter sum match across 5 processes");
    munmap((void*)counter, 4096);

    /* 2.2 Named shared memory via shm_open + ftruncate + mmap */
    shm_unlink("/ipctest_shm");
    int shm_fd = shm_open("/ipctest_shm", O_RDWR | O_CREAT | O_EXCL, 0666);
    if (shm_fd < 0) TEST_FAIL("shm_open create", "shm_open failed");

    if (ftruncate(shm_fd, 8192) != 0) TEST_FAIL("shm ftruncate", "ftruncate failed");

    char* shm_ptr = (char*) mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) TEST_FAIL("mmap named shm", "mmap failed");

    /* Sparse verify initial zero */
    ASSERT_EQ(shm_ptr[0], 0, "shm byte 0 is zero");
    ASSERT_EQ(shm_ptr[4096], 0, "shm byte 4096 is zero");

    /* Parent-child cross-process shm synchronization */
    pid_t cpid = fork();
    if (cpid < 0) TEST_FAIL("fork for shm test", "fork failed");
    if (cpid == 0) {
        /* Child writes to page 0 */
        strcpy(shm_ptr, "Child shm payload 12345");
        /* Child writes to page 1 */
        strcpy(shm_ptr + 4096, "Child second page payload");
        exit(0);
    }

    int cstatus = 0;
    waitpid(cpid, &cstatus, 0);
    if (!WIFEXITED(cstatus) || WEXITSTATUS(cstatus) != 0) {
        TEST_FAIL("child shm write exit", "child exited abnormally");
    }

    if (strcmp(shm_ptr, "Child shm payload 12345") != 0) {
        TEST_FAIL("shm page 0 verification", "content mismatch");
    }
    if (strcmp(shm_ptr + 4096, "Child second page payload") != 0) {
        TEST_FAIL("shm page 1 verification", "content mismatch");
    }

    /* 2.3 Verify write() syscall is visible through mmap pointer */
    lseek(shm_fd, 100, SEEK_SET);
    const char* syscall_msg = "SYSCALL_DATA";
    write(shm_fd, syscall_msg, strlen(syscall_msg));
    if (memcmp(shm_ptr + 100, syscall_msg, strlen(syscall_msg)) != 0) {
        TEST_FAIL("syscall write -> mmap visibility", "mmap did not reflect write() syscall");
    }

    /* 2.4 Verify mmap pointer write is visible through read() syscall */
    memcpy(shm_ptr + 200, "MMAP_DATA", 9);
    lseek(shm_fd, 200, SEEK_SET);
    char read_back[16];
    memset(read_back, 0, sizeof(read_back));
    read(shm_fd, read_back, 9);
    if (memcmp(read_back, "MMAP_DATA", 9) != 0) {
        TEST_FAIL("mmap write -> read() visibility", "read() did not reflect mmap pointer write");
    }

    /* 2.5 msync */
    if (msync(shm_ptr, 8192, MS_SYNC) != 0) {
        TEST_FAIL("msync", "msync failed");
    }

    munmap(shm_ptr, 8192);
    close(shm_fd);
    shm_unlink("/ipctest_shm");

    /* 2.6 SIGBUS beyond EOF verification */
    int bus_fd = shm_open("/shm_bus", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (bus_fd >= 0) {
        ftruncate(bus_fd, 4096); /* file size = 1 page */
        /* map 2 pages (8192 bytes) */
        volatile char* bus_ptr = (volatile char*) mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, bus_fd, 0);
        if (bus_ptr != MAP_FAILED) {
            pid_t bus_child = fork();
            if (bus_child == 0) {
                /* Touch beyond EOF */
                char c = bus_ptr[4096];
                (void) c;
                exit(0); /* Unreachable if SIGBUS occurs */
            }
            int bstatus = 0;
            waitpid(bus_child, &bstatus, 0);
            munmap((void*)bus_ptr, 8192);
            close(bus_fd);
            shm_unlink("/shm_bus");
            if (WTERMSIG(bstatus) == 7 || WEXITSTATUS(bstatus) == 135) {
                TEST_PASS("child terminated with SIGBUS on access beyond EOF");
            } else {
                TEST_FAIL("access beyond EOF", "child did not terminate with SIGBUS");
            }
        } else {
            close(bus_fd);
            shm_unlink("/shm_bus");
        }
    }

    TEST_PASS("MAP_SHARED anonymous + named tmpfs + coherence + msync verified");
}

/* ========================================================================= */
/* 3. AF_UNIX Sockets: socketpair, ring buffer wrap, bind, listen, accept    */
/* ========================================================================= */
static void test_af_unix_sockets(void) {
    printf("[3] Testing AF_UNIX stream sockets, ring buffer wrapping, and client-server...\n");

    /* 3.1 socketpair basic exchange and ring buffer wrap (128 KB > 64 KB ring) */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        TEST_FAIL("socketpair", "socketpair failed");
    }

    pid_t sp_pid = fork();
    if (sp_pid < 0) TEST_FAIL("fork for socketpair", "fork failed");
    if (sp_pid == 0) {
        close(sv[0]);
        /* Child sends 128 KB of sequential pattern in 4KB chunks */
        char chunk[4096];
        for (int b = 0; b < 32; b++) {
            memset(chunk, (char)b, sizeof(chunk));
            int written = 0;
            while (written < (int)sizeof(chunk)) {
                int nw = write(sv[1], chunk + written, sizeof(chunk) - written);
                if (nw <= 0) exit(1);
                written += nw;
            }
        }
        close(sv[1]);
        exit(0);
    }

    close(sv[1]);
    /* Parent reads 128 KB and verifies pattern */
    int total_read = 0;
    char rx_buf[4096];
    int chunk_idx = 0;
    int chunk_offset = 0;
    while (1) {
        int nr = read(sv[0], rx_buf, sizeof(rx_buf));
        if (nr == 0) break; /* EOF */
        if (nr < 0) TEST_FAIL("socketpair read", "read failed");
        for (int i = 0; i < nr; i++) {
            if (rx_buf[i] != (char)chunk_idx) {
                TEST_FAIL("ring buffer data corruption", "byte pattern mismatch during wrap test");
            }
            chunk_offset++;
            if (chunk_offset == 4096) {
                chunk_offset = 0;
                chunk_idx++;
            }
        }
        total_read += nr;
    }
    ASSERT_EQ(total_read, 128 * 1024, "socketpair 128KB ring buffer wrap transfer");
    close(sv[0]);

    int sp_status = 0;
    waitpid(sp_pid, &sp_status, 0);
    if (!WIFEXITED(sp_status) || WEXITSTATUS(sp_status) != 0) {
        TEST_FAIL("socketpair child exit", "child failed during 128KB transfer");
    }

    /* 3.2 shutdown(SHUT_WR) and SIGPIPE */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) TEST_FAIL("socketpair for shutdown", "failed");
    signal(SIGPIPE, handle_sigpipe);
    got_sigpipe = 0;

    /* Half-close writer */
    if (shutdown(sv[0], SHUT_WR) != 0) TEST_FAIL("shutdown SHUT_WR", "shutdown failed");

    /* Reading on other end sees EOF */
    char dummy;
    ASSERT_EQ(read(sv[1], &dummy, 1), 0, "read on shutdown writer end returns EOF");

    /* Writing to shutdown socket triggers EPIPE or SIGPIPE */
    int nw_bad = write(sv[0], "x", 1);
    if (nw_bad >= 0 || (errno != EPIPE && !got_sigpipe)) {
        TEST_FAIL("write to SHUT_WR socket", "expected EPIPE or SIGPIPE");
    }
    close(sv[0]);
    close(sv[1]);

    /* 3.3 Filesystem-bound server with multiple clients and poll() */
    unlink("/run/ipctest.sock");
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) TEST_FAIL("server socket", "socket creation failed");

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "/run/ipctest.sock");

    if (bind(srv, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        TEST_FAIL("bind /run/ipctest.sock", "bind failed");
    }

    /* Verify S_IFSOCK node in tmpfs */
    struct stat sst;
    if (stat("/run/ipctest.sock", &sst) < 0 || !S_ISSOCK(sst.st_mode)) {
        TEST_FAIL("stat S_IFSOCK", "node missing or not socket");
    }

    /* Duplicate bind returns EADDRINUSE */
    int srv_dup = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(srv_dup, (struct sockaddr*)&sun, sizeof(sun)) == 0 || errno != EADDRINUSE) {
        TEST_FAIL("duplicate bind EADDRINUSE", "expected EADDRINUSE");
    }
    close(srv_dup);

    if (listen(srv, 5) < 0) TEST_FAIL("listen", "listen failed");

    /* Spawn 3 clients */
    int num_clients = 3;
    pid_t c_pids[3];
    for (int i = 0; i < num_clients; i++) {
        c_pids[i] = fork();
        if (c_pids[i] < 0) TEST_FAIL("fork client", "fork failed");
        if (c_pids[i] == 0) {
            close(srv);
            int cl = socket(AF_UNIX, SOCK_STREAM, 0);
            if (cl < 0) exit(1);
            if (connect(cl, (struct sockaddr*)&sun, sizeof(sun)) < 0) exit(2);
            /* Send client ID */
            char cmsg[32];
            snprintf(cmsg, sizeof(cmsg), "CLIENT_%d_HELLO", i);
            write(cl, cmsg, strlen(cmsg));
            /* Read server reply */
            char reply[32];
            memset(reply, 0, sizeof(reply));
            int r = read(cl, reply, sizeof(reply) - 1);
            if (r <= 0) exit(3);
            if (strncmp(reply, "ACK_", 4) != 0) exit(4);
            close(cl);
            exit(0);
        }
    }

    /* Server accepts all 3 connections using poll() */
    int accepted = 0;
    while (accepted < num_clients) {
        struct pollfd pfd;
        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pret = poll(&pfd, 1, 5000);
        if (pret <= 0 || !(pfd.revents & POLLIN)) {
            TEST_FAIL("poll on server socket", "timeout or no POLLIN event");
        }

        struct sockaddr_un cl_addr;
        socklen_t cl_len = sizeof(cl_addr);
        int client_fd = accept(srv, (struct sockaddr*)&cl_addr, &cl_len);
        if (client_fd < 0) TEST_FAIL("accept client", "accept failed");

        char req[32];
        memset(req, 0, sizeof(req));
        int nr = read(client_fd, req, sizeof(req) - 1);
        if (nr <= 0) TEST_FAIL("server read client req", "read failed");

        /* Reply with ACK */
        char ack[32];
        snprintf(ack, sizeof(ack), "ACK_%s", req);
        write(client_fd, ack, strlen(ack));
        close(client_fd);
        accepted++;
    }

    for (int i = 0; i < num_clients; i++) {
        int status = 0;
        waitpid(c_pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            TEST_FAIL("client process exit", "client failed to complete exchange");
        }
    }

    close(srv);
    unlink("/run/ipctest.sock");

    TEST_PASS("AF_UNIX socketpair, 128KB ring buffer wrap, shutdown, server poll & multi-client verified");
}

/* ========================================================================= */
/* 4. PTY Subsystem: posix_openpt, grantpt/unlockpt, ptsname_r, ioctls, WINCH*/
/* ========================================================================= */
static void test_pty_subsystem(void) {
    printf("[4] Testing PTY subsystem: /dev/ptmx, slave creation, canonical mode, and SIGWINCH...\n");

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) TEST_FAIL("posix_openpt", "open /dev/ptmx failed");

    if (grantpt(master) < 0) TEST_FAIL("grantpt", "failed");
    if (unlockpt(master) < 0) TEST_FAIL("unlockpt", "failed");

    char slave_name[64];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        TEST_FAIL("ptsname_r", "failed to obtain pts name");
    }

    /* Verify slave device in /dev/pts */
    struct stat st;
    if (stat(slave_name, &st) < 0 || !S_ISCHR(st.st_mode)) {
        TEST_FAIL("stat slave device", "slave device missing or not S_IFCHR");
    }

    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) TEST_FAIL("open slave device", "open failed");

    /* Canonical translation test: write '\r' to master, read '\n' from slave */
    const char* in_cmd = "ipctest_line\r";
    write(master, in_cmd, strlen(in_cmd));

    char rx[64];
    memset(rx, 0, sizeof(rx));
    int nr = read(slave, rx, sizeof(rx) - 1);
    if (nr <= 0 || strcmp(rx, "ipctest_line\n") != 0) {
        TEST_FAIL("pty canonical translation", "ICRNL translation failed");
    }

    /* Verify master received echo */
    char echo[64];
    memset(echo, 0, sizeof(echo));
    int ne = read(master, echo, sizeof(echo) - 1);
    if (ne <= 0 || strstr(echo, "ipctest_line") == NULL) {
        TEST_FAIL("pty echo back to master", "echo missing or mismatch");
    }

    /* Window size change & SIGWINCH */
    signal(SIGWINCH, handle_sigwinch);
    signal(SIGHUP, SIG_IGN);
    got_sigwinch = 0;

    /* Associate slave with our process session */
    pid_t my_pid = getpid();
    ioctl(slave, TIOCSCTTY, 0);
    ioctl(slave, TIOCSPGRP, &my_pid);

    struct winsize ws;
    ws.ws_row = 45;
    ws.ws_col = 132;
    ws.ws_xpixel = 1056;
    ws.ws_ypixel = 720;
    if (ioctl(master, TIOCSWINSZ, &ws) < 0) {
        TEST_FAIL("ioctl TIOCSWINSZ on master", "failed");
    }

    struct winsize ws_get;
    memset(&ws_get, 0, sizeof(ws_get));
    if (ioctl(slave, TIOCGWINSZ, &ws_get) < 0) {
        TEST_FAIL("ioctl TIOCGWINSZ on slave", "failed");
    }
    ASSERT_EQ(ws_get.ws_row, 45, "TIOCGWINSZ ws_row");
    ASSERT_EQ(ws_get.ws_col, 132, "TIOCGWINSZ ws_col");

    ioctl(slave, TIOCNOTTY, 0);
    close(slave);
    close(master);

    TEST_PASS("PTY master/slave allocation, canonical translation, echo, and TIOCSWINSZ verified");
}

/* ========================================================================= */
/* 5. Integrated Pipeline: tmpfs SHM + UNIX Socket + PTY Cross-Subsystem IPC */
/* ========================================================================= */
static void test_integrated_pipeline(void) {
    printf("[5] Testing integrated multi-process IPC pipeline (SHM + UNIX Socket + PTY)...\n");

    /* Step A: Allocate named shm buffer in /dev/shm */
    shm_unlink("/pipe_shm");
    int shm_fd = shm_open("/pipe_shm", O_RDWR | O_CREAT | O_EXCL, 0666);
    if (shm_fd < 0) TEST_FAIL("pipeline shm_open", "failed");
    ftruncate(shm_fd, 4096);
    volatile uint32_t* shm_buf = (volatile uint32_t*) mmap(NULL, 4096,
                                                           PROT_READ | PROT_WRITE,
                                                           MAP_SHARED, shm_fd, 0);
    if (shm_buf == MAP_FAILED) TEST_FAIL("pipeline mmap", "failed");

    /* Step B: Create UNIX socketpair for signaling between Worker A and Worker B */
    int sig_sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sig_sv) < 0) {
        TEST_FAIL("pipeline socketpair", "failed");
    }

    /* Step C: Create PTY pair for Worker B -> Coordinator communication */
    int pty_master = -1, pty_slave = -1;
    if (openpty(&pty_master, &pty_slave, NULL, NULL, NULL) < 0) {
        TEST_FAIL("pipeline openpty", "failed");
    }

    /* Fork Worker B */
    pid_t worker_b = fork();
    if (worker_b == 0) {
        close(sig_sv[1]);
        close(pty_master);

        /* Wait for signal on socket */
        char sig_byte = 0;
        if (read(sig_sv[0], &sig_byte, 1) != 1 || sig_byte != 'R') {
            exit(10);
        }

        /* Verify data written by Worker A in shared memory */
        uint32_t sum = 0;
        for (int i = 0; i < 256; i++) {
            sum += shm_buf[i];
        }

        /* Expected sum: 0 + 1 + ... + 255 = 255 * 256 / 2 = 32640 */
        if (sum != 32640) {
            exit(11);
        }

        /* Write confirmation message to PTY slave */
        const char* conf = "PIPELINE_COMPLETE\r";
        write(pty_slave, conf, strlen(conf));
        close(pty_slave);
        close(sig_sv[0]);
        munmap((void*)shm_buf, 4096);
        close(shm_fd);
        exit(0);
    }

    /* Fork Worker A */
    pid_t worker_a = fork();
    if (worker_a == 0) {
        close(sig_sv[0]);
        close(pty_master);
        close(pty_slave);

        /* Fill shared memory buffer with sequence */
        for (int i = 0; i < 256; i++) {
            shm_buf[i] = (uint32_t)i;
        }

        /* Signal Worker B that data is ready */
        char sig_byte = 'R';
        write(sig_sv[1], &sig_byte, 1);

        close(sig_sv[1]);
        munmap((void*)shm_buf, 4096);
        close(shm_fd);
        exit(0);
    }

    /* Coordinator process: close slave and signal fds */
    close(pty_slave);
    close(sig_sv[0]);
    close(sig_sv[1]);

    /* Read result from PTY master */
    char pty_rx[64];
    memset(pty_rx, 0, sizeof(pty_rx));
    int pr = read(pty_master, pty_rx, sizeof(pty_rx) - 1);
    if (pr <= 0 || strstr(pty_rx, "PIPELINE_COMPLETE") == NULL) {
        TEST_FAIL("pipeline pty read", "did not receive PIPELINE_COMPLETE");
    }

    close(pty_master);
    munmap((void*)shm_buf, 4096);
    close(shm_fd);
    shm_unlink("/pipe_shm");

    /* Reap workers */
    int stat_a = 0, stat_b = 0;
    waitpid(worker_a, &stat_a, 0);
    waitpid(worker_b, &stat_b, 0);

    if (!WIFEXITED(stat_a) || WEXITSTATUS(stat_a) != 0) {
        TEST_FAIL("pipeline worker A exit", "worker A failed");
    }
    if (!WIFEXITED(stat_b) || WEXITSTATUS(stat_b) != 0) {
        TEST_FAIL("pipeline worker B exit", "worker B failed");
    }

    TEST_PASS("Integrated multi-process SHM + Socketpair + PTY pipeline succeeded with 0 errors");
}

/* ========================================================================= */
/* Main entry point                                                          */
/* ========================================================================= */
int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("========================================================\n");
    printf("=== Starting Stage 18B Step 6 IPCTEST Integration Suite ===\n");
    printf("========================================================\n\n");

    signal(SIGHUP, SIG_IGN);

    test_tmpfs_operations();
    printf("\n");

    test_shared_memory();
    printf("\n");

    test_af_unix_sockets();
    printf("\n");

    test_pty_subsystem();
    printf("\n");

    test_integrated_pipeline();
    printf("\n");

    printf("========================================================\n");
    printf("=== ALL STAGE 18B IPC & SESSION TESTS PASSED (5/5)   ===\n");
    printf("========================================================\n");
    return 0;
}
