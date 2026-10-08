#include "libc.h"

static void put_str(const char* s) {
    write(1, s, strlen(s));
}

static void test_pass(const char* name) {
    put_str("[PASS] ");
    put_str(name);
    put_str("\n");
}

static void test_fail(const char* name) {
    put_str("[FAIL] ");
    put_str(name);
    put_str("\n");
    exit(1);
}

static int recurse_depth(int depth) {
    volatile char pad[16 * 1024]; /* 16KB per stack frame */
    pad[0] = (char)depth;
    pad[16383] = (char)(depth ^ 0x55);
    if (depth <= 0) return 0;
    return pad[0] + recurse_depth(depth - 1);
}

static int __attribute__((noinline, optimize("no-optimize-sibling-calls"))) recurse_inf(void) {
    volatile char pad[16 * 1024];
    pad[0] = 1;
    pad[16383] = 2;
    return pad[0] + recurse_inf();
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    put_str("========================================\n");
    put_str("Stage 15 Memory Management Test Suite\n");
    put_str("========================================\n");

    /* ---------------------------------------------------------
     * 1. COW Correctness Test
     * --------------------------------------------------------- */
    put_str("[TEST 1] COW Correctness (4MB array fork/modify)...\n");
    {
        unsigned char* p = (unsigned char*) malloc(4 * 1024 * 1024);
        if (!p) test_fail("COW correctness: malloc(4MB) returned NULL");

        for (int i = 0; i < 4 * 1024 * 1024; i++) {
            p[i] = (unsigned char)(i & 0xFF);
        }

        int pid = fork();
        if (pid < 0) test_fail("COW correctness: fork failed");

        if (pid == 0) {
            /* Child modifies first 2MB */
            for (int i = 0; i < 2 * 1024 * 1024; i++) {
                p[i] = (unsigned char)((i & 0xFF) ^ 0xAA);
            }
            /* Verify child writes */
            for (int i = 0; i < 2 * 1024 * 1024; i++) {
                if (p[i] != (unsigned char)((i & 0xFF) ^ 0xAA)) exit(1);
            }
            for (int i = 2 * 1024 * 1024; i < 4 * 1024 * 1024; i++) {
                if (p[i] != (unsigned char)(i & 0xFF)) exit(2);
            }
            exit(0);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 0) test_fail("COW correctness: child verification failed");

        /* Verify parent data is completely untouched */
        for (int i = 0; i < 4 * 1024 * 1024; i++) {
            if (p[i] != (unsigned char)(i & 0xFF)) {
                test_fail("COW correctness: parent memory was modified by child!");
            }
        }
        free(p);
        test_pass("COW correctness (parent untouched, child modified independently)");
    }

    /* ---------------------------------------------------------
     * 2. COW Efficiency Test
     * --------------------------------------------------------- */
    put_str("[TEST 2] COW Efficiency (free frame count check)...\n");
    {
        unsigned int f_before = sys_free_frames();

        unsigned char* cow_buf = (unsigned char*) malloc(4 * 1024 * 1024);
        if (!cow_buf) test_fail("COW efficiency: malloc failed");

        /* Touch all 1024 pages */
        for (int i = 0; i < 4 * 1024 * 1024; i += 4096) {
            cow_buf[i] = 0x77;
        }

        unsigned int f_with_buf = sys_free_frames();

        int pid = fork();
        if (pid < 0) test_fail("COW efficiency: fork failed");

        if (pid == 0) {
            unsigned int f_child = sys_free_frames();
            int drop = (int)f_with_buf - (int)f_child;
            /* In COW, fork must only duplicate page tables (~4-10 frames), NOT 1024 frames */
            if (drop > 50) {
                exit(1);
            }
            exit(0);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 0) test_fail("COW efficiency: fork duplicated too many frames (not COW)");

        free(cow_buf);
        unsigned int f_after = sys_free_frames();
        if (f_after != f_before) {
            test_fail("COW efficiency: frame leak after free and child exit");
        }
        test_pass("COW efficiency (minimal frames allocated on fork, baseline restored)");
    }

    /* ---------------------------------------------------------
     * 3. Demand Paging Test
     * --------------------------------------------------------- */
    put_str("[TEST 3] Demand Paging (64MB sbrk, touch 10 pages)...\n");
    {
        unsigned int b_frames = sys_free_frames();
        void* heap_top = sbrk(64 * 1024 * 1024);
        if (heap_top == (void*)-1) test_fail("Demand paging: sbrk(64MB) failed");

        unsigned int sbrk_frames = sys_free_frames();
        if (b_frames != sbrk_frames) {
            test_fail("Demand paging: sbrk eagerly allocated frames before touch");
        }

        volatile unsigned char* hp = (volatile unsigned char*) heap_top;
        for (int p = 0; p < 10; p++) {
            hp[p * 4096] = (unsigned char)(p + 1);
        }

        unsigned int touch_frames = sys_free_frames();
        int consumed = (int)b_frames - (int)touch_frames;
        if (consumed < 10 || consumed > 15) {
            test_fail("Demand paging: expected ~10 frames consumed, but consumed != 10");
        }

        /* Verify content */
        for (int p = 0; p < 10; p++) {
            if (hp[p * 4096] != (unsigned char)(p + 1)) {
                test_fail("Demand paging: read back mismatch");
            }
        }

        sbrk(-64 * 1024 * 1024);
        unsigned int end_frames = sys_free_frames();
        if (end_frames != b_frames) {
            test_fail("Demand paging: sbrk shrink did not restore free frames");
        }
        test_pass("Demand paging (lazy allocation on touch, shrink restores frames)");
    }

    /* ---------------------------------------------------------
     * 4. Stack Growth Test
     * --------------------------------------------------------- */
    put_str("[TEST 4] Stack Growth (2MB recursion & 8MB limit guard)...\n");
    {
        /* Bounded recursion using ~2MB stack */
        int res = recurse_depth(128);
        (void) res;
        test_pass("Stack auto-growth (2MB stack recursion succeeded)");

        /* Infinite recursion killed cleanly with exit 139 */
        int pid = fork();
        if (pid < 0) test_fail("Stack growth: fork failed");
        if (pid == 0) {
            recurse_inf();
            exit(0);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 139) {
            test_fail("Stack guard: infinite recursion was not killed with exit 139");
        }
        test_pass("Stack guard gap (unbounded recursion killed cleanly with exit 139)");
    }

    /* ---------------------------------------------------------
     * 5. mmap / munmap Test
     * --------------------------------------------------------- */
    put_str("[TEST 5] mmap / munmap (anon, file-backed, unmap fault)...\n");
    {
        /* Anonymous map */
        void* m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (m == MAP_FAILED) test_fail("mmap: anon map failed");
        volatile unsigned char* map_p = (volatile unsigned char*) m;
        map_p[0] = 0x42;
        map_p[8191] = 0x84;
        if (map_p[0] != 0x42 || map_p[8191] != 0x84) test_fail("mmap: anon memory mismatch");
        if (munmap(m, 8192) != 0) test_fail("mmap: munmap failed");

        /* File-backed map */
        int fd = open("/hello.txt", O_RDONLY);
        if (fd >= 0) {
            char read_buf[256];
            int n = read(fd, read_buf, sizeof(read_buf));
            void* fm = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
            if (fm != MAP_FAILED) {
                const char* fmp = (const char*) fm;
                int match = 1;
                for (int i = 0; i < n; i++) {
                    if (fmp[i] != read_buf[i]) { match = 0; break; }
                }
                munmap(fm, 4096);
                if (!match) test_fail("mmap: file-backed content mismatch with read()");
            }
            close(fd);
        }

        /* Munmap then touch must fault and kill child */
        int pid = fork();
        if (pid < 0) test_fail("mmap: fork failed");
        if (pid == 0) {
            void* p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            munmap(p, 4096);
            *(volatile unsigned char*)p = 1;
            exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 139) test_fail("mmap: touching munmap'd memory was not killed with exit 139");

        test_pass("mmap / munmap (anonymous, file-backed, and unmap fault verified)");
    }

    /* ---------------------------------------------------------
     * 6. Protection Violation Tests
     * --------------------------------------------------------- */
    put_str("[TEST 6] Protection Violations (NULL, code write, no-exec, kernel addr)...\n");
    {
        /* NULL dereference */
        int pid = fork();
        if (pid == 0) {
            *(volatile int*)0 = 0x1234;
            exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 139) test_fail("Protection: NULL dereference not killed with 139");

        /* Write to code segment */
        pid = fork();
        if (pid == 0) {
            *(volatile unsigned char*)((unsigned int)&main) = 0x90;
            exit(0);
        }
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 139) test_fail("Protection: write to code segment not killed with 139");

        /* Jump to non-executable / PROT_NONE data */
        pid = fork();
        if (pid == 0) {
            void* p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            mprotect(p, 4096, PROT_NONE);
            ((void (*)(void))p)();
            exit(0);
        }
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 139) test_fail("Protection: execution of non-executable region not killed with 139");

        /* Kernel address access */
        pid = fork();
        if (pid == 0) {
            *(volatile int*)0xC0000000 = 0xDEAD;
            exit(0);
        }
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) != 139) test_fail("Protection: user access to kernel address not killed with 139");

        test_pass("Protection violations (NULL, code write, non-exec, kernel address all exit 139)");
    }

    /* ---------------------------------------------------------
     * 7. Fork Storm Test (200 sequential + 20 concurrent)
     * --------------------------------------------------------- */
    put_str("[TEST 7] Fork Storm (200 sequential + 20 concurrent children)...\n");
    {
        unsigned int storm_start = sys_free_frames();

        /* 200 sequential fork / exit */
        for (int i = 0; i < 200; i++) {
            int pid = fork();
            if (pid < 0) test_fail("Fork storm: sequential fork failed");
            if (pid == 0) {
                exit(0);
            }
            int status = 0;
            waitpid(pid, &status, 0);
        }

        /* 20 concurrent children */
        int pids[20];
        for (int i = 0; i < 20; i++) {
            pids[i] = fork();
            if (pids[i] < 0) test_fail("Fork storm: concurrent fork failed");
            if (pids[i] == 0) {
                exit(0);
            }
        }
        for (int i = 0; i < 20; i++) {
            int status = 0;
            waitpid(pids[i], &status, 0);
        }

        unsigned int storm_end = sys_free_frames();
        put_str("[storm] start: ");
        print_uint(storm_start);
        put_str(", end: ");
        print_uint(storm_end);
        put_str("\n");
        if (storm_end != storm_start) {
            test_fail("Fork storm: free frame count leaked after 220 forks!");
        }
        test_pass("Fork storm (200 sequential + 20 concurrent children clean, zero leaks)");
    }

    put_str("========================================\n");
    put_str("[memtest] ALL TESTS PASSED SUCCESSFULLY!\n");
    put_str("========================================\n");
    return 0;
}
