#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s (line %d, errno=%d: %s)\n", msg, __LINE__, errno, strerror(errno)); \
        failed++; \
    } else { \
        printf("  [PASS] %s\n", msg); \
        passed++; \
    } \
} while (0)

int main(int argc, char *argv[]) {
    /* Child handler for O_CLOEXEC across exec check */
    if (argc >= 3 && strcmp(argv[1], "--check-closed") == 0) {
        int target_fd = atoi(argv[2]);
        char b[1];
        int n = read(target_fd, b, 1);
        if (n == -1 && errno == EBADF) {
            return 0; /* Success: fd is indeed closed */
        }
        return 1; /* Failed: fd leaked across exec */
    }

    printf("=== Starting flagtest (libc/newlib flag translation tests) ===\n");
    int passed = 0;
    int failed = 0;

    /* -------------------------------------------------------------
     * Test 1: open("/dev/input/kbd0", O_RDONLY | O_NONBLOCK)
     *         read() returns -1 with errno == EAGAIN
     * ------------------------------------------------------------- */
    printf("--- Test 1: Non-blocking device read (O_NONBLOCK) ---\n");
    int kbd_fd = open("/dev/input/kbd0", O_RDONLY | O_NONBLOCK);
    ASSERT(kbd_fd >= 0, "open /dev/input/kbd0 with O_NONBLOCK succeeded");
    if (kbd_fd >= 0) {
        char kbuf[64];
        int n = read(kbd_fd, kbuf, sizeof(kbuf));
        ASSERT(n == -1, "non-blocking read returns -1 when no key event");
        ASSERT(errno == EAGAIN, "errno is EAGAIN on empty non-blocking read");
        close(kbd_fd);
    }

    /* -------------------------------------------------------------
     * Test 2: fcntl(fd, F_GETFL) & F_SETFL for O_NONBLOCK
     * ------------------------------------------------------------- */
    printf("--- Test 2: fcntl F_GETFL / F_SETFL (O_NONBLOCK) ---\n");
    int test_fd = open("/tmp/flag_fgetfl.tmp", O_RDWR | O_CREAT | O_TRUNC, 0666);
    ASSERT(test_fd >= 0, "open /tmp/flag_fgetfl.tmp succeeded");
    if (test_fd >= 0) {
        int fl = fcntl(test_fd, F_GETFL);
        ASSERT(fl >= 0, "fcntl F_GETFL succeeded");
        ASSERT((fl & O_NONBLOCK) == 0, "O_NONBLOCK initially clear");

        int ret = fcntl(test_fd, F_SETFL, fl | O_NONBLOCK);
        ASSERT(ret == 0, "fcntl F_SETFL setting O_NONBLOCK succeeded");

        fl = fcntl(test_fd, F_GETFL);
        ASSERT((fl & O_NONBLOCK) != 0, "F_GETFL reflects O_NONBLOCK set");

        ret = fcntl(test_fd, F_SETFL, fl & ~O_NONBLOCK);
        ASSERT(ret == 0, "fcntl F_SETFL clearing O_NONBLOCK succeeded");

        fl = fcntl(test_fd, F_GETFL);
        ASSERT((fl & O_NONBLOCK) == 0, "F_GETFL reflects O_NONBLOCK cleared");
        close(test_fd);
    }

    int nb_fd = open("/tmp/flag_fgetfl.tmp", O_RDONLY | O_NONBLOCK);
    ASSERT(nb_fd >= 0, "open with O_NONBLOCK directly succeeded");
    if (nb_fd >= 0) {
        int fl = fcntl(nb_fd, F_GETFL);
        ASSERT((fl & O_NONBLOCK) != 0, "F_GETFL reports O_NONBLOCK after open");
        close(nb_fd);
    }

    /* -------------------------------------------------------------
     * Test 3: O_CREAT & O_TRUNC
     * ------------------------------------------------------------- */
    printf("--- Test 3: O_CREAT & O_TRUNC ---\n");
    unlink("/tmp/flag_trunc.tmp");
    int cr_fd = open("/tmp/flag_trunc.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    ASSERT(cr_fd >= 0, "O_CREAT created non-existent file");
    if (cr_fd >= 0) {
        int w = write(cr_fd, "1234567890", 10);
        ASSERT(w == 10, "wrote 10 bytes to file");
        close(cr_fd);
    }

    int tr_fd = open("/tmp/flag_trunc.tmp", O_WRONLY | O_TRUNC, 0666);
    ASSERT(tr_fd >= 0, "open existing file with O_TRUNC succeeded");
    if (tr_fd >= 0) {
        int w = write(tr_fd, "ABC", 3);
        ASSERT(w == 3, "wrote 3 bytes to truncated file");
        close(tr_fd);
    }

    int chk_fd = open("/tmp/flag_trunc.tmp", O_RDONLY);
    ASSERT(chk_fd >= 0, "open truncated file for verify");
    if (chk_fd >= 0) {
        char rbuf[32] = {0};
        int rd = read(chk_fd, rbuf, sizeof(rbuf));
        ASSERT(rd == 3, "truncated file size is exactly 3 bytes");
        ASSERT(strcmp(rbuf, "ABC") == 0, "content matches new write");
        close(chk_fd);
    }

    /* -------------------------------------------------------------
     * Test 4: O_EXCL | O_CREAT
     * ------------------------------------------------------------- */
    printf("--- Test 4: O_EXCL | O_CREAT ---\n");
    /* /tmp/flag_trunc.tmp already exists from Test 3 */
    int excl_fail = open("/tmp/flag_trunc.tmp", O_RDWR | O_CREAT | O_EXCL, 0666);
    ASSERT(excl_fail == -1, "open existing file with O_CREAT|O_EXCL fails");
    ASSERT(errno == EEXIST, "errno is EEXIST when file already exists");

    unlink("/tmp/flag_excl_new.tmp");
    int excl_ok = open("/tmp/flag_excl_new.tmp", O_RDWR | O_CREAT | O_EXCL, 0666);
    ASSERT(excl_ok >= 0, "open non-existent file with O_CREAT|O_EXCL succeeds");
    if (excl_ok >= 0) close(excl_ok);

    /* -------------------------------------------------------------
     * Test 5: O_APPEND (writes land at EOF even after lseek to 0)
     * ------------------------------------------------------------- */
    printf("--- Test 5: O_APPEND behavior ---\n");
    unlink("/tmp/flag_append.tmp");
    int app_init = open("/tmp/flag_append.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    ASSERT(app_init >= 0, "created append test file");
    if (app_init >= 0) {
        write(app_init, "HEAD", 4);
        close(app_init);
    }

    int app_fd = open("/tmp/flag_append.tmp", O_WRONLY | O_APPEND);
    ASSERT(app_fd >= 0, "open with O_APPEND succeeded");
    if (app_fd >= 0) {
        int fl = fcntl(app_fd, F_GETFL);
        ASSERT((fl & O_APPEND) != 0, "F_GETFL reflects O_APPEND");

        /* Seek to beginning of file */
        off_t sk = lseek(app_fd, 0, SEEK_SET);
        ASSERT(sk == 0, "lseek to 0 succeeded");

        /* Write must still go to EOF because of O_APPEND */
        int w = write(app_fd, "TAIL", 4);
        ASSERT(w == 4, "write with O_APPEND succeeded");
        close(app_fd);
    }

    int app_chk = open("/tmp/flag_append.tmp", O_RDONLY);
    ASSERT(app_chk >= 0, "open append file to verify");
    if (app_chk >= 0) {
        char buf[32] = {0};
        int rd = read(app_chk, buf, sizeof(buf));
        ASSERT(rd == 8, "file size is 8 bytes");
        ASSERT(strcmp(buf, "HEADTAIL") == 0, "write landed at EOF despite lseek(0)");
        close(app_chk);
    }

    /* -------------------------------------------------------------
     * Test 6: Access Modes (O_RDONLY, O_WRONLY, O_RDWR)
     * ------------------------------------------------------------- */
    printf("--- Test 6: Access Modes (O_RDONLY, O_WRONLY, O_RDWR) ---\n");
    int ro_fd = open("/tmp/flag_append.tmp", O_RDONLY);
    ASSERT(ro_fd >= 0, "open O_RDONLY succeeded");
    if (ro_fd >= 0) {
        int w = write(ro_fd, "NOPE", 4);
        ASSERT(w == -1, "write to O_RDONLY fd returns -1");
        ASSERT(errno == EBADF, "errno is EBADF on write to O_RDONLY");
        close(ro_fd);
    }

    int wo_fd = open("/tmp/flag_append.tmp", O_WRONLY);
    ASSERT(wo_fd >= 0, "open O_WRONLY succeeded");
    if (wo_fd >= 0) {
        char tmp[16];
        int r = read(wo_fd, tmp, sizeof(tmp));
        ASSERT(r == -1, "read from O_WRONLY fd returns -1");
        ASSERT(errno == EBADF, "errno is EBADF on read from O_WRONLY");
        close(wo_fd);
    }

    int rw_fd = open("/tmp/flag_append.tmp", O_RDWR);
    ASSERT(rw_fd >= 0, "open O_RDWR succeeded");
    if (rw_fd >= 0) {
        char tmp[16];
        int r = read(rw_fd, tmp, 4);
        ASSERT(r == 4, "read from O_RDWR succeeded");
        int w = write(rw_fd, "PLUS", 4);
        ASSERT(w == 4, "write to O_RDWR succeeded");
        close(rw_fd);
    }

    /* -------------------------------------------------------------
     * Test 7: O_CLOEXEC across exec
     * ------------------------------------------------------------- */
    printf("--- Test 7: O_CLOEXEC across exec ---\n");
    int clo_fd = open("/tmp/flag_append.tmp", O_RDONLY | O_CLOEXEC);
    ASSERT(clo_fd >= 0, "open with O_CLOEXEC succeeded");
    if (clo_fd >= 0) {
        int fd_fl = fcntl(clo_fd, F_GETFD);
        ASSERT((fd_fl & FD_CLOEXEC) != 0, "FD_CLOEXEC flag is set in descriptor flags");

        pid_t pid = fork();
        ASSERT(pid >= 0, "fork child process succeeded");
        if (pid == 0) {
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", clo_fd);
            char *child_args[] = { "flagtest.elf", "--check-closed", fd_str, NULL };
            execvp("flagtest.elf", child_args);
            execvp("/bin/flagtest.elf", child_args);
            execvp("/flagtest.elf", child_args);
            _exit(127);
        } else {
            int status = 0;
            waitpid(pid, &status, 0);
            ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "O_CLOEXEC fd was successfully closed across exec");
        }
        close(clo_fd);
    }

    printf("\n=== flagtest results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
