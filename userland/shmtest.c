#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAILED: %s (line %d): %s\n", msg, __LINE__, strerror(errno)); \
        exit(1); \
    } \
} while (0)

static void test_anon_shared(void) {
    printf("[1] Testing MAP_SHARED | MAP_ANON across fork with atomic increments...\n");
    volatile uint32_t* counter = (volatile uint32_t*) mmap(NULL, 4096,
                                                           PROT_READ | PROT_WRITE,
                                                           MAP_SHARED | MAP_ANON,
                                                           -1, 0);
    TEST_ASSERT(counter != MAP_FAILED, "mmap MAP_SHARED | MAP_ANON");
    *counter = 0;

    int nchildren = 4;
    for (int i = 0; i < nchildren; i++) {
        pid_t pid = fork();
        TEST_ASSERT(pid >= 0, "fork child");
        if (pid == 0) {
            for (int k = 0; k < 100000; k++) {
                __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
            }
            exit(0);
        }
    }

    /* Parent increments 100,000 times as well */
    for (int k = 0; k < 100000; k++) {
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
    }

    for (int i = 0; i < nchildren; i++) {
        int status = 0;
        wait(&status);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exited cleanly");
    }

    uint32_t final_val = *counter;
    printf("    Final counter value: %lu (expected 500000)\n", (unsigned long)final_val);
    TEST_ASSERT(final_val == 500000, "atomic increments match exact expected value");

    TEST_ASSERT(munmap((void*)counter, 4096) == 0, "munmap anon shared");
    printf("    PASSED: anon shared + atomic increments\n");
}

static void test_fat_rejection(void) {
    printf("[2] Testing MAP_SHARED rejection on FAT file (-ENODEV)...\n");
    int fd = open("/hello.txt", O_RDONLY);
    TEST_ASSERT(fd >= 0, "open /hello.txt");

    void* ptr = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    TEST_ASSERT(ptr == MAP_FAILED, "mmap MAP_SHARED on FAT must fail");
    TEST_ASSERT(errno == ENODEV, "errno must be ENODEV on FAT MAP_SHARED");
    close(fd);
    printf("    PASSED: FAT MAP_SHARED rejected with ENODEV\n");
}

static void test_tmpfs_shm(void) {
    printf("[3] Testing shm_open, ftruncate, MAP_SHARED on tmpfs...\n");
    shm_unlink("/shm_test");

    int fd = shm_open("/shm_test", O_RDWR | O_CREAT | O_EXCL, 0666);
    TEST_ASSERT(fd >= 0, "shm_open create");

    TEST_ASSERT(ftruncate(fd, 8192) == 0, "ftruncate to 8192");

    char* ptr = (char*) mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT(ptr != MAP_FAILED, "mmap tmpfs MAP_SHARED");

    /* Sparse read: initial contents must be zero */
    TEST_ASSERT(ptr[0] == 0 && ptr[4096] == 0, "sparse read initial zeros");

    /* Write via mmap, read via read() */
    const char* msg = "Hello from shm mmap!";
    strcpy(ptr, msg);

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    lseek(fd, 0, SEEK_SET);
    int nr = read(fd, read_buf, strlen(msg));
    TEST_ASSERT(nr == (int)strlen(msg), "read from tmpfs file");
    TEST_ASSERT(strcmp(read_buf, msg) == 0, "read sees mmap write");

    /* Write via write(), read via mmap */
    const char* msg2 = "Written via write()!";
    lseek(fd, 4096, SEEK_SET);
    int nw = write(fd, msg2, strlen(msg2) + 1);
    TEST_ASSERT(nw == (int)strlen(msg2) + 1, "write to tmpfs page 1");
    TEST_ASSERT(strcmp(ptr + 4096, msg2) == 0, "mmap sees write()");

    /* Test msync (no-op) */
    TEST_ASSERT(msync(ptr, 8192, MS_SYNC) == 0, "msync returns 0");

    /* Share with another process via fork */
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        strcpy(ptr + 100, "Child wrote here!");
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT(strcmp(ptr + 100, "Child wrote here!") == 0, "parent sees child write to shm");

    /* Cleanup */
    TEST_ASSERT(munmap(ptr, 8192) == 0, "munmap tmpfs");
    close(fd);
    TEST_ASSERT(shm_unlink("/shm_test") == 0, "shm_unlink");
    printf("    PASSED: tmpfs shm_open, ftruncate, and bidirectional sharing\n");
}

static void test_sigbus_eof(void) {
    printf("[4] Testing SIGBUS on access beyond EOF...\n");
    int fd = shm_open("/shm_sigbus", O_RDWR | O_CREAT | O_TRUNC, 0666);
    TEST_ASSERT(fd >= 0, "shm_open");
    TEST_ASSERT(ftruncate(fd, 2048) == 0, "ftruncate to 2048");

    /* Map 4096 (1 page). File size is 2048. Access to byte 100 is valid. Access to page 1 (4096) is beyond EOF. */
    volatile char* ptr = (volatile char*) mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT(ptr != MAP_FAILED, "mmap 8192 with file size 2048");

    /* Access within file works */
    ptr[0] = 'A';
    TEST_ASSERT(ptr[0] == 'A', "access within EOF");

    /* Fork child to touch beyond EOF (offset 4096 >= size 2048) */
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork child for SIGBUS");
    if (pid == 0) {
        /* Touch beyond EOF */
        char c = ptr[4096];
        (void) c;
        exit(0); /* Should not be reached */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    printf("    Child exit status: %d (WTERMSIG=%d, WEXITSTATUS=%d)\n",
           status, WTERMSIG(status), WEXITSTATUS(status));
    TEST_ASSERT(WTERMSIG(status) == 7 || WEXITSTATUS(status) == 135,
                "child terminated with SIGBUS / status 135");

    munmap((void*)ptr, 8192);
    close(fd);
    shm_unlink("/shm_sigbus");
    printf("    PASSED: SIGBUS on access beyond EOF\n");
}

static void test_truncate_shrink_sigbus(void) {
    printf("[5] Testing truncate-shrink with live mappings raises SIGBUS...\n");
    int fd = shm_open("/shm_shrink", O_RDWR | O_CREAT | O_TRUNC, 0666);
    TEST_ASSERT(fd >= 0, "shm_open");
    TEST_ASSERT(ftruncate(fd, 8192) == 0, "ftruncate 8192");

    volatile char* ptr = (volatile char*) mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT(ptr != MAP_FAILED, "mmap 8192");

    /* Touch both pages while file size is 8192 */
    ptr[0] = 'X';
    ptr[4096] = 'Y';
    TEST_ASSERT(ptr[0] == 'X' && ptr[4096] == 'Y', "touched pages 0 and 1");

    /* Truncate file down to 4096: page 1 should be unmapped */
    TEST_ASSERT(ftruncate(fd, 4096) == 0, "truncate shrink to 4096");

    /* Page 0 is still valid */
    TEST_ASSERT(ptr[0] == 'X', "page 0 still accessible");

    /* Fork child to touch page 1 -> must trigger SIGBUS */
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        char c = ptr[4096];
        (void) c;
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    printf("    Child exit status after shrink: %d (WTERMSIG=%d, WEXITSTATUS=%d)\n",
           status, WTERMSIG(status), WEXITSTATUS(status));
    TEST_ASSERT(WTERMSIG(status) == 7 || WEXITSTATUS(status) == 135,
                "child terminated with SIGBUS / status 135 after truncate-shrink");

    munmap((void*)ptr, 8192);
    close(fd);
    shm_unlink("/shm_shrink");
    printf("    PASSED: truncate-shrink unmapping and SIGBUS\n");
}

int main(void) {
    printf("======================================\n");
    printf("  STAGE 18B STEP 2: MAP_SHARED TEST  \n");
    printf("======================================\n");

    test_anon_shared();
    test_fat_rejection();
    test_tmpfs_shm();
    test_sigbus_eof();
    test_truncate_shrink_sigbus();

    printf("\n>>> ALL STAGE 18B STEP 2 TESTS PASSED! <<<\n");
    return 0;
}
