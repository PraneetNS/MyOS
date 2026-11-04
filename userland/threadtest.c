#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdint.h>

/* --- Subtest 1: Churn (1000 thread create/join) --- */
static void* churn_worker(void* arg) {
    (void)arg;
    return (void*)42;
}

static void test_churn(void) {
    printf("[threadtest] Subtest 1: Thread create/join churn (1000 threads)...\n");
    for (int i = 0; i < 1000; i++) {
        pthread_t th;
        int r = pthread_create(&th, NULL, churn_worker, NULL);
        if (r != 0) {
            printf("FAIL: pthread_create failed at iteration %d (err=%d)\n", i, r);
            exit(1);
        }
        void* ret = NULL;
        r = pthread_join(th, &ret);
        if (r != 0 || (int)(uintptr_t)ret != 42) {
            printf("FAIL: pthread_join failed at iteration %d\n", i);
            exit(1);
        }
    }
    printf("PASS: thread create/join churn (1000 threads)\n");
}

/* --- Subtest 2: Mutex, Condition Variable, Barrier Contention --- */
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  test_cond  = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t test_barrier;
static volatile int shared_counter = 0;
static volatile int cond_ready = 0;

static void* mutex_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&test_mutex);
        shared_counter++;
        pthread_mutex_unlock(&test_mutex);
    }
    return NULL;
}

static void* barrier_worker(void* arg) {
    (void)arg;
    pthread_barrier_wait(&test_barrier);
    return NULL;
}

static void* cond_worker(void* arg) {
    (void)arg;
    pthread_mutex_lock(&test_mutex);
    while (!cond_ready) {
        pthread_cond_wait(&test_cond, &test_mutex);
    }
    pthread_mutex_unlock(&test_mutex);
    return NULL;
}

static void test_sync_primitives(void) {
    printf("[threadtest] Subtest 2: Mutex, Cond, Barrier contention...\n");

    /* 1. Mutex contention: 4 threads x 10,000 increments = 40,000 */
    shared_counter = 0;
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, mutex_worker, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    if (shared_counter != 40000) {
        printf("FAIL: mutex counter mismatch: %d != 40000\n", shared_counter);
        exit(1);
    }

    /* 2. Barrier */
    pthread_barrier_init(&test_barrier, NULL, 4);
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, barrier_worker, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    pthread_barrier_destroy(&test_barrier);

    /* 3. Cond var */
    cond_ready = 0;
    pthread_t cth;
    pthread_create(&cth, NULL, cond_worker, NULL);
    usleep(10000);
    pthread_mutex_lock(&test_mutex);
    cond_ready = 1;
    pthread_cond_signal(&test_cond);
    pthread_mutex_unlock(&test_mutex);
    pthread_join(cth, NULL);

    printf("PASS: mutex, condition variable, barrier contention\n");
}

/* --- Subtest 3: TLS Variable Independence --- */
static __thread int tls_var = 0;
static volatile int tls_fail = 0;

static void* tls_worker(void* arg) {
    int my_val = (int)(uintptr_t)arg;
    tls_var = my_val;
    for (volatile int i = 0; i < 50000; i++);
    if (tls_var != my_val) {
        tls_fail = 1;
    }
    return NULL;
}

static void test_tls(void) {
    printf("[threadtest] Subtest 3: TLS variable independence...\n");
    tls_var = 1234;
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, tls_worker, (void*)(uintptr_t)(100 + i));
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    if (tls_fail || tls_var != 1234) {
        printf("FAIL: TLS variable corrupted across threads\n");
        exit(1);
    }
    printf("PASS: TLS variable independence\n");
}

/* --- Subtest 4: Signal Mask and tgkill --- */
static volatile int sigusr1_received = 0;

static void sigusr1_handler(int sig) {
    (void)sig;
    sigusr1_received = 1;
}

struct sig_arg {
    pthread_t target_tid;
    pid_t target_kernel_tid;
};

static void* sig_target_worker(void* arg) {
    pid_t* my_tid = (pid_t*)arg;
    *my_tid = (pid_t)syscall(SYS_gettid);

    /* Block SIGUSR1 on this thread */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* Spin waiting to ensure tgkill was sent while blocked */
    usleep(50000);
    if (sigusr1_received != 0) {
        printf("FAIL: signal delivered while blocked\n");
        exit(1);
    }

    /* Now unblock SIGUSR1: signal should be delivered immediately */
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    for (volatile int i = 0; i < 10000 && !sigusr1_received; i++);

    if (!sigusr1_received) {
        printf("FAIL: signal not delivered after unblocking\n");
        exit(1);
    }
    return NULL;
}

static void test_signals(void) {
    printf("[threadtest] Subtest 4: Thread signal masks and tgkill...\n");
    signal(SIGUSR1, sigusr1_handler);
    sigusr1_received = 0;

    pid_t target_tid = 0;
    pthread_t th;
    pthread_create(&th, NULL, sig_target_worker, &target_tid);

    while (target_tid == 0) usleep(1000);

    /* Send SIGUSR1 targeted specifically to thread target_tid */
    int r = syscall(SYS_tgkill, getpid(), target_tid, SIGUSR1);
    if (r != 0) {
        printf("FAIL: tgkill failed (err=%d)\n", errno);
        exit(1);
    }

    pthread_join(th, NULL);
    printf("PASS: thread signal masks and tgkill\n");
}

/* --- Subtest 5: Robust Futex List --- */
struct robust_list_head_t {
    void* list;
    long futex_offset;
    void* list_op_pending;
};

static void test_robust_futex(void) {
    printf("[threadtest] Subtest 5: Robust futex list syscalls...\n");
    struct robust_list_head_t head;
    memset(&head, 0, sizeof(head));
    head.list = &head;
    head.futex_offset = 0;

    int r = syscall(273 /* __NR_set_robust_list */, &head, sizeof(head));
    if (r != 0) {
        printf("FAIL: set_robust_list returned %d\n", r);
        exit(1);
    }

    void* retrieved_head = NULL;
    size_t retrieved_len = 0;
    r = syscall(274 /* __NR_get_robust_list */, 0, &retrieved_head, &retrieved_len);
    if (r != 0 || retrieved_head != &head || retrieved_len != sizeof(head)) {
        printf("FAIL: get_robust_list failed\n");
        exit(1);
    }

    /* Reset robust list head */
    syscall(273, NULL, 0);
    printf("PASS: robust futex list syscalls\n");
}

#define SYS_AGENT_CREATE 1002
#define SYS_AGENT_ATTACH 1003

typedef struct {
    char prefix[64];
} test_agent_path_rule_t;

typedef struct {
    uint32_t mask;
    char principal[32];
    uint32_t num_fs_read;
    test_agent_path_rule_t fs_read[8];
    uint32_t num_fs_write;
    test_agent_path_rule_t fs_write[8];
    uint32_t num_exec;
    test_agent_path_rule_t exec[8];
    uint32_t num_connect;
    test_agent_path_rule_t connect[8];
    uint32_t num_bus_call;
    test_agent_path_rule_t bus_call[8];
    uint32_t num_bus_provide;
    test_agent_path_rule_t bus_provide[8];
    uint32_t num_window_observe;
    test_agent_path_rule_t window_observe[8];
    uint32_t num_window_act;
    test_agent_path_rule_t window_act[8];
    uint32_t num_mem_read;
    test_agent_path_rule_t mem_read[8];
    uint32_t num_mem_write;
    test_agent_path_rule_t mem_write[8];
    uint32_t num_net_http;
    test_agent_path_rule_t net_http[8];
    uint32_t model_backends;
} test_agent_caps_t;

typedef struct {
    uint32_t max_cpu_ticks;
    uint32_t max_mem_pages;
    uint32_t max_procs;
    uint32_t max_threads;
    uint32_t max_tokens;
    uint32_t max_net_bytes;
} test_agent_budget_t;

/* --- Subtest 6: Agent max_threads Denial --- */
static void* thread_sleep_worker(void* arg) {
    (void)arg;
    usleep(200000);
    return NULL;
}

static void test_agent_thread_limit(void) {
    printf("[threadtest] Subtest 6: Agent max_threads budget denial...\n");
    pid_t pid = fork();
    if (pid == 0) {
        test_agent_caps_t caps;
        memset(&caps, 0, sizeof(caps));
        caps.mask = 0xFFFFFFFF; /* all caps */

        test_agent_budget_t budget;
        memset(&budget, 0, sizeof(budget));
        budget.max_threads = 4; /* limit to 4 threads */

        int ag_id = (int)syscall(SYS_AGENT_CREATE, "th_limit_agent", &caps, &budget);
        if (ag_id <= 0) {
            printf("FAIL: agent_create failed (id=%d)\n", ag_id);
            exit(1);
        }
        int r = (int)syscall(SYS_AGENT_ATTACH, ag_id);
        if (r != 0) {
            printf("FAIL: agent_attach failed (r=%d)\n", r);
            exit(1);
        }

        /* The main thread counts as thread 1. Creating 3 more should succeed. */
        pthread_t th[5];
        int spawned = 0;
        for (int i = 0; i < 3; i++) {
            if (pthread_create(&th[i], NULL, thread_sleep_worker, NULL) == 0) {
                spawned++;
            }
        }
        if (spawned != 3) {
            printf("FAIL: could not spawn up to max_threads limit (%d/3)\n", spawned);
            exit(1);
        }

        /* The next thread creation must exceed max_threads and fail with EAGAIN */
        int err = pthread_create(&th[3], NULL, thread_sleep_worker, NULL);
        if (err != EAGAIN) {
            printf("FAIL: thread clone beyond max_threads did not return EAGAIN (err=%d)\n", err);
            exit(1);
        }

        for (int i = 0; i < spawned; i++) {
            pthread_join(th[i], NULL);
        }
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: agent max_threads child exited with status %d\n", WEXITSTATUS(st));
        exit(1);
    }
    printf("PASS: agent max_threads budget denial (-EAGAIN)\n");
}

/* --- Subtest 7: Kernel Stack Guard & kstack_hwm --- */
static void test_kstack_hwm(void) {
    printf("[threadtest] Subtest 7: Kernel stack HWM check...\n");
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) {
        printf("FAIL: could not open /proc/self/status\n");
        exit(1);
    }
    char line[256];
    int hwm = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "kstack_hwm:", 11) == 0) {
            hwm = atoi(line + 11);
            break;
        }
    }
    fclose(f);

    if (hwm <= 0) {
        printf("FAIL: invalid kstack_hwm: %d\n", hwm);
        exit(1);
    }
    /* 32KB stack = 32768 bytes. 50% is 16384 bytes. */
    if (hwm > 16384) {
        printf("FAIL: kstack_hwm %d exceeds 50%% of stack (16384)\n", hwm);
        exit(1);
    }
    printf("PASS: kstack_hwm=%d (well within 50%% threshold)\n", hwm);
}

/* --- Subtest 8: Zero Memory Frame Leak --- */
static int get_free_frames(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    int free_frames = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "FramesFree:", 11) == 0) {
            free_frames = atoi(line + 11);
            break;
        }
    }
    fclose(f);
    return free_frames;
}

static void test_frame_leak(void) {
    printf("[threadtest] Subtest 8: Zero memory frame leak across thread churn...\n");
    int before = get_free_frames();

    for (int i = 0; i < 200; i++) {
        pthread_t th;
        pthread_create(&th, NULL, churn_worker, NULL);
        pthread_join(th, NULL);
    }

    sched_yield();
    usleep(10000);

    int after = get_free_frames();
    int diff = abs(after - before);
    if (diff > 4) {
        printf("FAIL: memory frame leak detected: before=%d, after=%d, diff=%d\n", before, after, diff);
        exit(1);
    }
    printf("PASS: zero memory frame leak (diff=%d frames)\n", diff);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("[threadtest] starting POSIX threads validation on native Linux ABI...\n");

    test_churn();
    test_sync_primitives();
    test_tls();
    test_signals();
    test_robust_futex();
    test_agent_thread_limit();
    test_kstack_hwm();
    test_frame_leak();

    printf("\nALL THREAD TESTS PASSED (8/8)\n");
    return 0;
}
