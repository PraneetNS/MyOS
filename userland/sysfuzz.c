#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <agent.h>
#include <myos_syscalls.h>

#define CANARY_PATH "/canary.txt"
#define CANARY_DATA "SECRET_CANARY_INTEGRITY_TOKEN_12345\n"

static int sys_symlink(const char* target, const char* linkpath) {
    long ret;
    asm volatile (
        "mov $88, %%rax\n\t"
        "mov %1, %%rdi\n\t"
        "mov %2, %%rsi\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "g"(target), "g"(linkpath)
        : "rcx", "r11", "memory"
    );
    return (int) ret;
}

/* Deterministic 64-bit xorshift PRNG */
static uint64_t s_prng_state = 0x853c49e6748fea9bULL;

static uint64_t prng_next(void) {
    uint64_t x = s_prng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_prng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t prng_range(uint64_t min, uint64_t max) {
    if (min >= max) return min;
    return min + (prng_next() % (max - min + 1));
}

static int read_free_frames(void) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char* p = strstr(buf, "FramesFree:\t");
    if (!p) {
        p = strstr(buf, "FramesFree: ");
        if (!p) return -1;
        p += 12;
    } else {
        p += 12;
    }
    return atoi(p);
}

/* Known Linux & MyOS syscall numbers to fuzz */
static const uint64_t s_syscall_numbers[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 28, 32, 33, 34, 35, 37, 38, 39, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 61, 63, 72, 73, 74, 75,
    76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,
    94, 95, 96, 97, 99, 100, 102, 104, 107, 108, 109, 110, 111, 112,
    137, 138, 186, 202, 217, 218, 228, 229, 230, 234, 257, 258, 260, 262,
    263, 264, 265, 266, 267, 268, 269, 270, 271, 273, 274, 280, 288, 292, 293, 302,
    316, 318, 332,
    /* MyOS custom syscalls */
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009,
    1013, 1014, 1015, 1016, 1017, 1018, 1019, 1020, 1021, 1099,
    /* Unknown / invalid syscall numbers */
    500, 888, 999, 1200, 0x10000, 0x7fffffff, 0xffffffff
};
#define NUM_SYSCALL_NUMS (sizeof(s_syscall_numbers) / sizeof(s_syscall_numbers[0]))

static const uint64_t s_bad_pointers[] = {
    0,
    1,
    0x1000,
    0x7fffffffffULL,
    0xffffffff80000000ULL,
    0xffffffff80100000ULL,
    0xdeadbeefULL,
    0xffffffffffffffffULL,
    (uint64_t)-1LL
};
#define NUM_BAD_PTRS (sizeof(s_bad_pointers) / sizeof(s_bad_pointers[0]))

static const uint64_t s_sizes[] = {
    0, 1, 2, 4, 8, 16, 64, 256, 512, 1024, 4096, 8192,
    0x7fffffffULL, 0xffffffffULL, 0xffffffffffffffffULL
};
#define NUM_SIZES (sizeof(s_sizes) / sizeof(s_sizes[0]))

static const int64_t s_fds[] = {
    -100, -1, 3, 4, 10, 31, 32, 63, 64, 100, 1024, 0x7fffffff
};
#define NUM_FDS (sizeof(s_fds) / sizeof(s_fds[0]))

static char s_long_comp[300];
static char s_long_path[1100];

static const char* s_unconf_paths[16];
static int s_num_unconf_paths = 0;

static const char* s_sandboxed_paths[32];
static int s_num_sandboxed_paths = 0;

static void init_fuzz_paths(void) {
    memset(s_long_comp, 'A', 255);
    s_long_comp[255] = '\0';

    memset(s_long_path, 'B', 1024);
    s_long_path[0] = '/';
    s_long_path[1024] = '\0';

    /* Unconfined paths: safe scratch paths, never /canary.txt */
    s_unconf_paths[s_num_unconf_paths++] = "";
    s_unconf_paths[s_num_unconf_paths++] = "/tmp/scratch1.txt";
    s_unconf_paths[s_num_unconf_paths++] = "/tmp/scratch2.bin";
    s_unconf_paths[s_num_unconf_paths++] = s_long_comp;
    s_unconf_paths[s_num_unconf_paths++] = s_long_path;

    /* Sandboxed paths: actively attempt escape targets and canary access */
    s_sandboxed_paths[s_num_sandboxed_paths++] = "";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/.";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/..";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "//../..//../canary.txt";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/canary.txt";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/canary.txt/../canary.txt";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/tmp/sandbox/sub/../../canary.txt";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/tmp/sandbox/symlink_to_canary";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/self/mem";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/1/mem";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/1/maps";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/1/status";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/agents";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/agents/1/status";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/proc/agents/log";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/run/approval.sock";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/run/intent.sock";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/run/display.sock";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/etc/init.conf";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/tmp/sandbox/test.txt";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/tmp/sandbox/deep/nested/path/test.dat";
    s_sandboxed_paths[s_num_sandboxed_paths++] = "/tmp/sandbox/\xe6\xbc\xa2\xe5\xad\x97_\xf0\x9f\x98\x8a.txt"; /* UTF-8 */
    s_sandboxed_paths[s_num_sandboxed_paths++] = s_long_comp;
    s_sandboxed_paths[s_num_sandboxed_paths++] = s_long_path;
}

static uint64_t get_random_arg(int is_sandboxed) {
    int choice = (int) prng_range(0, 5);
    switch (choice) {
        case 0: /* Bad pointer */
            return s_bad_pointers[prng_range(0, NUM_BAD_PTRS - 1)];
        case 1: /* Huge / corner size */
            return s_sizes[prng_range(0, NUM_SIZES - 1)];
        case 2: /* File descriptor */
            return (uint64_t) s_fds[prng_range(0, NUM_FDS - 1)];
        case 3: /* Path pointer */
            if (is_sandboxed) {
                return (uint64_t)(uintptr_t) s_sandboxed_paths[prng_range(0, s_num_sandboxed_paths - 1)];
            } else {
                return (uint64_t)(uintptr_t) s_unconf_paths[prng_range(0, s_num_unconf_paths - 1)];
            }
        case 4: /* Flag bits */
            return prng_next() & 0xFFFFFFFFULL;
        default:
            return prng_next();
    }
}

static inline long invoke_syscall6(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "mov %5, %%r10\n\t"
        "mov %6, %%r8\n\t"
        "mov %7, %%r9\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "g"(nr), "g"(a1), "g"(a2), "g"(a3), "g"(a4), "g"(a5), "g"(a6)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long invoke_syscall0(uint64_t nr) {
    return invoke_syscall6(nr, 0, 0, 0, 0, 0, 0);
}
static inline long invoke_syscall1(uint64_t nr, uint64_t a1) {
    return invoke_syscall6(nr, a1, 0, 0, 0, 0, 0);
}
static inline long invoke_syscall2(uint64_t nr, uint64_t a1, uint64_t a2) {
    return invoke_syscall6(nr, a1, a2, 0, 0, 0, 0);
}
static inline long invoke_syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    return invoke_syscall6(nr, a1, a2, a3, 0, 0, 0);
}
static inline long invoke_syscall4(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    return invoke_syscall6(nr, a1, a2, a3, a4, 0, 0);
}

static int verify_canary(void) {
    int fd = open(CANARY_PATH, O_RDONLY);
    if (fd < 0) return 0;
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (strcmp(buf, CANARY_DATA) == 0);
}

static inline int is_blacklisted_syscall(uint64_t nr) {
    /* Process lifecycle & execution */
    if (nr == 15 || nr == 56 || nr == 57 || nr == 58 || nr == 59 || nr == 60 || nr == 62) return 1;
    if (nr == 131 || nr == 158 || nr == 169 || nr == 200 || nr == 231 || nr == 234) return 1;
    /* Blocking / sleep syscalls */
    if (nr == 0 || nr == 7 || nr == 23 || nr == 34 || nr == 35 || nr == 43 || nr == 45) return 1;
    if (nr == 61 || nr == 130 || nr == 230 || nr == 270 || nr == 271 || nr == 288) return 1;
    /* Daemons & special control */
    if (nr == 1004 || nr == 1005 || nr == 1009 || nr == 1013 || nr == 1014 || nr == 1017 || nr == 1099) return 1;
    return 0;
}

static volatile int s_thread_stop = 0;

static int thread_fuzz_worker(void* arg) {
    (void) arg;
    uint32_t futex_word = 0;
    while (!s_thread_stop) {
        /* Concurrently exercise thread-related syscalls */
        uint64_t tnr = prng_next() % 5;
        if (tnr == 0) {
            invoke_syscall0(186 /* gettid */);
        } else if (tnr == 1) {
            invoke_syscall0(24 /* sched_yield */);
        } else if (tnr == 2) {
            invoke_syscall3(202 /* futex */, (uint64_t)&futex_word, 1 /* FUTEX_WAKE */, 1);
        } else if (tnr == 3) {
            invoke_syscall1(218 /* set_tid_address */, (uint64_t)&futex_word);
        } else {
            invoke_syscall0(1000 /* SYS_TICKS */);
        }
    }
    return 0;
}

static int create_fuzz_thread(int (*fn)(void*), void* arg) {
    void* stack = malloc(65536);
    if (!stack) return -1;
    void* stack_top = (void*)((uintptr_t)stack + 65536 - 16);
    unsigned long flags = 0x00000100 /* CLONE_VM */ |
                          0x00000200 /* CLONE_FS */ |
                          0x00000400 /* CLONE_FILES */ |
                          0x00000800 /* CLONE_SIGHAND */ |
                          0x00010000 /* CLONE_THREAD */ |
                          0x00080000 /* CLONE_SYSVSEM */;
    long ret;
    asm volatile (
        "mov $56, %%rax\n\t"
        "mov %1, %%rdi\n\t"
        "mov %2, %%rsi\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "g"(flags), "g"(stack_top)
        : "rcx", "r11", "memory"
    );
    if (ret == 0) {
        fn(arg);
        /* SYS_exit */
        asm volatile (
            "mov $60, %%rax\n\t"
            "xor %%rdi, %%rdi\n\t"
            "syscall\n\t"
            : : : "memory"
        );
    }
    return (int)ret;
}

static int run_sandboxed_escape_audit(int canary_inherited_fd) {
    int escapes = 0;

    /* 1. Directly opening outside files */
    int fd = open(CANARY_PATH, O_RDONLY);
    if (fd >= 0) { printf("ESCAPE: open(%s, RDONLY) succeeded\n", CANARY_PATH); close(fd); escapes++; }
    fd = open(CANARY_PATH, O_WRONLY);
    if (fd >= 0) { printf("ESCAPE: open(%s, WRONLY) succeeded\n", CANARY_PATH); close(fd); escapes++; }

    /* 2. Symlink pointing outside */
    fd = open("/tmp/sandbox/symlink_to_canary", O_RDONLY);
    if (fd >= 0) { printf("ESCAPE: open symlink pointing to canary succeeded\n"); close(fd); escapes++; }

    /* 3. Dot-dot traversal */
    fd = open("/tmp/sandbox/../canary.txt", O_RDONLY);
    if (fd >= 0) { printf("ESCAPE: dot-dot traversal open succeeded\n"); close(fd); escapes++; }

    /* 4. Truncate / chmod / utimensat on outside path */
    long r = invoke_syscall2(76 /* truncate */, (uint64_t)CANARY_PATH, 0);
    if (r >= 0) { printf("ESCAPE: truncate(%s) succeeded\n", CANARY_PATH); escapes++; }
    r = invoke_syscall2(90 /* chmod */, (uint64_t)CANARY_PATH, 0777);
    if (r >= 0) { printf("ESCAPE: chmod(%s) succeeded\n", CANARY_PATH); escapes++; }
    r = invoke_syscall4(280 /* utimensat */, (uint64_t)-100 /* AT_FDCWD */, (uint64_t)CANARY_PATH, 0, 0);
    if (r >= 0) { printf("ESCAPE: utimensat(%s) succeeded\n", CANARY_PATH); escapes++; }

    /* 5. Rename / link across sandbox boundary */
    r = invoke_syscall2(82 /* rename */, (uint64_t)"/tmp/sandbox/sub", (uint64_t)"/tmp/escaped_dir");
    if (r >= 0) { printf("ESCAPE: rename out of sandbox succeeded\n"); escapes++; }
    r = invoke_syscall2(82 /* rename */, (uint64_t)CANARY_PATH, (uint64_t)"/tmp/sandbox/stolen_canary");
    if (r >= 0) { printf("ESCAPE: rename into sandbox succeeded\n"); escapes++; }
    r = invoke_syscall2(86 /* link */, (uint64_t)CANARY_PATH, (uint64_t)"/tmp/sandbox/hardlink");
    if (r >= 0) { printf("ESCAPE: hard link to canary succeeded\n"); escapes++; }

    /* 6. Process management & signaling across boundaries */
    r = invoke_syscall2(109 /* setpgid */, 1, 1);
    if (r >= 0) { printf("ESCAPE: setpgid(1, 1) succeeded\n"); escapes++; }
    r = invoke_syscall2(62 /* kill */, 1, 0);
    if (r >= 0) { printf("ESCAPE: kill(1, 0) succeeded\n"); escapes++; }

    /* 7. Procfs cross-process isolation & mem prohibited */
    fd = open("/proc/1/status", O_RDONLY);
    if (fd >= 0) { printf("ESCAPE: open(/proc/1/status) succeeded\n"); close(fd); escapes++; }
    fd = open("/proc/self/mem", O_RDONLY);
    if (fd >= 0) { printf("ESCAPE: open(/proc/self/mem) succeeded\n"); close(fd); escapes++; }
    fd = open("/proc/agents", O_RDONLY);
    if (fd >= 0) { printf("ESCAPE: open(/proc/agents) succeeded\n"); close(fd); escapes++; }

    /* 8. Inherited FD check: parent opened canary on fd >= 3, must be sanitized/closed in child */
    if (canary_inherited_fd >= 3) {
        char buf[32];
        int n = read(canary_inherited_fd, buf, sizeof(buf));
        if (n > 0) {
            printf("ESCAPE: read on inherited outside fd %d succeeded\n", canary_inherited_fd);
            escapes++;
        }
    }

    /* 9. 1000-range restricted MyOS syscalls */
    static const int s_restricted_1000[] = {
        1003, /* SYS_AGENT_ATTACH */
        1004, /* SYS_AGENT_KILL */
        1005, /* SYS_AGENT_SUSPEND */
        1006, /* SYS_AGENT_RESUME */
        1009, /* SYS_BUS_DAEMON_REGISTER */
        1011, /* SYS_AGENT_CHARGE_TOKENS */
        1013, /* SYS_MODEL_DAEMON_REGISTER */
        1015, /* SYS_AGENT_CHARGE_NET */
        1017, /* SYS_NET_DAEMON_REGISTER */
        1018, /* SYS_TEST_HARDENING */
        1099  /* kstack recursion debug */
    };
    for (size_t i = 0; i < sizeof(s_restricted_1000)/sizeof(s_restricted_1000[0]); i++) {
        r = invoke_syscall6(s_restricted_1000[i], 1, 0, 0, 0, 0, 0);
        if (r >= 0) {
            printf("ESCAPE: restricted syscall %d returned %ld (expected -EPERM)\n", s_restricted_1000[i], r);
            escapes++;
        }
    }

    return escapes;
}

static int run_fuzz_loop(int iterations, int is_sandboxed, int canary_fd) {
    int escapes = 0;
    s_thread_stop = 0;

    /* Launch concurrent fuzz worker thread */
    int th_tid = create_fuzz_thread(thread_fuzz_worker, NULL);
    if (th_tid > 0) {
        printf("[sysfuzz] Spawned concurrent fuzz thread (TID %d)\n", th_tid);
    }

    for (int i = 0; i < iterations; i++) {
        uint64_t nr;
        if (prng_range(0, 9) < 8) {
            nr = s_syscall_numbers[prng_range(0, NUM_SYSCALL_NUMS - 1)];
        } else {
            nr = prng_next() % 2000;
        }

        if (is_blacklisted_syscall(nr)) {
            continue;
        }

        uint64_t a1 = get_random_arg(is_sandboxed);
        uint64_t a2 = get_random_arg(is_sandboxed);
        uint64_t a3 = get_random_arg(is_sandboxed);
        uint64_t a4 = get_random_arg(is_sandboxed);
        uint64_t a5 = get_random_arg(is_sandboxed);
        uint64_t a6 = get_random_arg(is_sandboxed);

        long ret = invoke_syscall6(nr, a1, a2, a3, a4, a5, a6);
        (void) ret;

        /* If sandboxed, any ability to open/read canary directly is an escape */
        if (is_sandboxed && (i % 100 == 0)) {
            int fd = open(CANARY_PATH, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                escapes++;
            }
        }

        /* If sandboxed, calling restricted 1000-range syscalls must never succeed */
        if (is_sandboxed && (nr == 1003 || nr == 1004 || nr == 1005 || nr == 1006 ||
                             nr == 1009 || nr == 1011 || nr == 1013 || nr == 1015 ||
                             nr == 1017 || nr == 1018 || nr == 1099)) {
            if (ret == 0) {
                escapes++;
            }
        }
    }

    s_thread_stop = 1;
    invoke_syscall0(24 /* sched_yield */);

    if (is_sandboxed) {
        escapes += run_sandboxed_escape_audit(canary_fd);
    }

    return escapes;
}

int main(int argc, char** argv) {
    int total_iterations = 20000;
    uint64_t seed = 0x853c49e6748fea9bULL;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--iterations") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            total_iterations = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--seed") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--long") == 0) {
            total_iterations = 100000;
        } else if (argv[i][0] != '-' && atoi(argv[i]) > 0) {
            total_iterations = atoi(argv[i]);
        }
    }

    if (seed != 0) {
        s_prng_state = seed;
    }

    printf("============================================================\n");
    printf("[sysfuzz] Linux ABI Syscall Fuzzer & Sandbox Escape Auditor\n");
    printf("[sysfuzz] Target iterations: %d (seed: 0x%lx)\n", total_iterations, s_prng_state);
    printf("============================================================\n");

    init_fuzz_paths();

    /* 1. Setup canary file outside sandbox */
    int cfd = open(CANARY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (cfd < 0) {
        printf("FAIL: cannot create canary file\n");
        return 1;
    }
    write(cfd, CANARY_DATA, strlen(CANARY_DATA));
    close(cfd);

    if (!verify_canary()) {
        printf("FAIL: canary initialization verification failed\n");
        return 1;
    }
    printf("[sysfuzz] Canary established at %s (integrity confirmed)\n", CANARY_PATH);

    /* Setup sandbox directory */
    mkdir("/tmp", 0755);
    mkdir("/tmp/sandbox", 0755);
    mkdir("/tmp/sandbox/sub", 0755);
    sys_symlink(CANARY_PATH, "/tmp/sandbox/symlink_to_canary");

    int frames_before = read_free_frames();
    printf("[sysfuzz] Baseline free physical frames: %d\n", frames_before);

    int total_calls = 0;
    int crashes = 0;
    int escapes = 0;
    int leaks = 0;

    /* 2. Run unconfined fuzzing pass */
    int unconfined_count = total_iterations / 2;
    printf("[sysfuzz] Phase 1: Unconfined fuzzing (%d iterations)...\n", unconfined_count);
    pid_t pid = fork();
    if (pid == 0) {
        int esc = run_fuzz_loop(unconfined_count, 0, -1);
        exit(esc);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("FAIL: unconfined worker crashed (status=0x%x)\n", status);
            crashes++;
        } else {
            total_calls += unconfined_count;
            printf("[sysfuzz] Phase 1 completed successfully (%d calls executed)\n", unconfined_count);
        }
    }

    /* 3. Run sandboxed agent fuzzing pass */
    int sandboxed_count = total_iterations / 2;
    printf("[sysfuzz] Phase 2: Sandboxed agent fuzzing (%d iterations)...\n", sandboxed_count);

    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = CAP_FS_READ | CAP_FS_WRITE | CAP_EXEC;
    caps.num_fs_read = 1;
    strncpy(caps.fs_read[0].prefix, "/tmp/sandbox", MAX_PATH_PREFIX_LEN - 1);
    caps.num_fs_write = 1;
    strncpy(caps.fs_write[0].prefix, "/tmp/sandbox", MAX_PATH_PREFIX_LEN - 1);
    caps.num_exec = 1;
    strncpy(caps.exec[0].prefix, "/bin", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    budget.max_procs = 4;
    budget.max_mem_pages = 256;

    int ag_id = agent_create("fuzz_sandbox_agent", &caps, &budget);
    if (ag_id <= 0) {
        printf("FAIL: agent_create failed (ag_id=%d)\n", ag_id);
        return 1;
    }
    printf("[sysfuzz] Created restricted sandbox agent %d (allowed prefix: /tmp/sandbox)\n", ag_id);

    /* Open canary in parent to test inherited fd closure upon agent attach */
    int parent_canary_fd = open(CANARY_PATH, O_RDONLY);

    pid = fork();
    if (pid == 0) {
        if (agent_attach(ag_id) != 0) {
            exit(99);
        }
        int esc = run_fuzz_loop(sandboxed_count, 1, parent_canary_fd);
        exit(esc);
    } else if (pid > 0) {
        if (parent_canary_fd >= 0) close(parent_canary_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (WEXITSTATUS(status) == 99) {
                printf("FAIL: agent_attach failed in child\n");
            } else {
                printf("FAIL: sandboxed worker failed or reported escapes (status=0x%x)\n", status);
            }
            escapes++;
        } else {
            total_calls += sandboxed_count;
            printf("[sysfuzz] Phase 2 completed successfully (%d calls executed)\n", sandboxed_count);
        }
    }

    agent_kill(ag_id);

    /* 4. Final canary verification */
    if (!verify_canary()) {
        printf("FAIL: canary corrupted or modified by sandboxed calls!\n");
        escapes++;
    } else {
        printf("[sysfuzz] Canary integrity verified: pristine!\n");
    }

    /* 5. Resource leak verification */
    unlink("/tmp/sandbox/symlink_to_canary");
    unlink(CANARY_PATH);

    int frames_after = read_free_frames();
    printf("[sysfuzz] Final free physical frames: %d (delta: %d frames)\n",
           frames_after, frames_after - frames_before);

    /* Allow up to one 256KB (64-frame) kheap chunk growth for audit logging and buffer cache */
    if (frames_before > 0 && frames_after > 0) {
        int delta = frames_before - frames_after;
        if (delta > 64 || delta < -64) {
            printf("FAIL: potential frame leak detected (delta=%d)\n", delta);
            leaks++;
        } else {
            printf("[sysfuzz] Memory frame counts returned to baseline (delta=%d frames)\n", delta);
        }
    }

    printf("============================================================\n");
    printf("[sysfuzz] SUMMARY:\n");
    printf("  Calls executed : %d\n", total_calls);
    printf("  Crashes        : %d\n", crashes);
    printf("  Leaks          : %d\n", leaks);
    printf("  Escapes        : %d\n", escapes);
    printf("============================================================\n");

    if (crashes == 0 && leaks == 0 && escapes == 0 && total_calls >= total_iterations) {
        printf("PASS: SYSFUZZ PASSED WITH ZERO CRASHES, LEAKS, OR ESCAPES\n");
        return 0;
    } else {
        printf("FAIL: SYSFUZZ DETECTED FAILURES\n");
        return 1;
    }
}
