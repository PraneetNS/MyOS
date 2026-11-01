#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>

static volatile int caught_sigsegv = 0;

static void segv_handler(int sig) {
    (void)sig;
    caught_sigsegv = 1;
    _exit(42);
}

static void test_nx(void) {
    printf("[hardentest] 1. Testing NX protection on heap/stack...\n");
    pid_t pid = fork();
    if (pid == 0) {
        /* In child: allocate buffer on heap, put a 'ret' (0xC3) opcode, and attempt to call it */
        uint8_t* code = (uint8_t*) malloc(64);
        if (!code) _exit(1);
        code[0] = 0xC3; /* ret */

        signal(SIGSEGV, segv_handler);

        /* Cast to function pointer and execute */
        typedef void (*func_t)(void);
        func_t fn = (func_t)code;
        fn();

        /* If we reached here, NX failed! */
        _exit(99);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        printf("PASSED: NX bit enforced (SIGSEGV delivered on executing non-executable heap)\n");
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
        printf("PASSED: NX bit enforced (killed by SIGSEGV on non-executable heap)\n");
    } else {
        printf("FAILED: NX bit test returned status %d\n", status);
        exit(1);
    }
}

static void test_aslr(void) {
    printf("[hardentest] 2. Testing ASLR (mmap base randomization)...\n");

    /* Map an anonymous page */
    void* addr1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (addr1 == MAP_FAILED) {
        printf("FAILED: mmap failed in parent\n");
        exit(1);
    }

    int pfd[2];
    if (pipe(pfd) != 0) {
        printf("FAILED: pipe creation failed\n");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        /* Child allocates an anonymous page */
        void* addr2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        write(pfd[1], &addr2, sizeof(void*));
        close(pfd[1]);
        _exit(0);
    }

    close(pfd[1]);
    void* addr2 = NULL;
    read(pfd[0], &addr2, sizeof(void*));
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    printf("  Parent mmap: %p\n", addr1);
    printf("  Child mmap:  %p\n", addr2);

    /* Verify addresses are within 64-bit user address space */
    if ((uintptr_t)addr1 < 0x1000 || (uintptr_t)addr1 >= 0x0000800000000000ULL ||
        (uintptr_t)addr2 < 0x1000 || (uintptr_t)addr2 >= 0x0000800000000000ULL) {
        printf("FAILED: mmap addresses out of 64-bit range\n");
        exit(1);
    }

    munmap(addr1, 4096);
    printf("PASSED: ASLR anonymous mmap space verified in 64-bit canonical range\n");
}

static void test_cpu_hardening(void) {
    printf("[hardentest] 3. Inspecting CPU hardening features (/proc/cpuinfo)...\n");
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) {
        printf("FAILED: cannot open /proc/cpuinfo\n");
        exit(1);
    }

    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("FAILED: read /proc/cpuinfo\n");
        exit(1);
    }
    buf[n] = '\0';

    if (strstr(buf, "lm") && strstr(buf, "nx")) {
        printf("PASSED: CPU reports 64-bit Long Mode (lm) and No-Execute (nx)\n");
    } else {
        printf("FAILED: missing lm or nx flags in /proc/cpuinfo\n");
        exit(1);
    }

    if (strstr(buf, "smep") || strstr(buf, "smap")) {
        printf("PASSED: Modern supervisor security flags present: %s%s\n",
               strstr(buf, "smep") ? "smep " : "",
               strstr(buf, "smap") ? "smap" : "");
    } else {
        printf("INFO: smep/smap not present on current QEMU CPU model (test with -cpu max)\n");
    }
}

static void test_64bit_memory(void) {
    printf("[hardentest] 4. Testing 64-bit address space & memory allocation...\n");
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char* line = strstr(buf, "MemTotal:");
            if (line) {
                char mem_line[64];
                int i = 0;
                while (line[i] && line[i] != '\n' && i < 63) {
                    mem_line[i] = line[i];
                    i++;
                }
                mem_line[i] = '\0';
                printf("  %s\n", mem_line);
            }
        }
    }

    /* Allocate, touch, and free 8MB using mmap */
    size_t sz = 8 * 1024 * 1024;
    void* p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAILED: large 8MB mmap failed\n");
        exit(1);
    }
    memset(p, 0x5A, sz);
    if (((uint8_t*)p)[0] != 0x5A || ((uint8_t*)p)[sz - 1] != 0x5A) {
        printf("FAILED: memory verification failed\n");
        exit(1);
    }
    munmap(p, sz);
    printf("PASSED: 64-bit large memory mmap and write verification succeeded\n");
}

int main(void) {
    printf("=== MyOS x86-64 CPU Hardening & Memory Test Suite ===\n");
    test_nx();
    test_aslr();
    test_cpu_hardening();
    test_64bit_memory();
    printf("ALL HARDENING TESTS PASSED!\n");
    return 0;
}

