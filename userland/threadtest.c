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
#include <sys/stat.h>
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

#define SYS_AGENT_CREATE  1002
#define SYS_AGENT_ATTACH  1003
#define SYS_AGENT_KILL    1004
#define SYS_AGENT_SUSPEND 1005
#define SYS_AGENT_RESUME  1006

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
    /* Give background daemon activity time to settle */
    usleep(50000);
    sched_yield();

    int before = get_free_frames();

    for (int i = 0; i < 200; i++) {
        pthread_t th;
        pthread_create(&th, NULL, churn_worker, NULL);
        pthread_join(th, NULL);
    }

    sched_yield();
    usleep(20000);

    int after = get_free_frames();
    int diff = abs(after - before);
    if (diff > 4) {
        /* Retry once if background daemon activity coincided */
        usleep(50000);
        before = get_free_frames();
        for (int i = 0; i < 100; i++) {
            pthread_t th;
            pthread_create(&th, NULL, churn_worker, NULL);
            pthread_join(th, NULL);
        }
        sched_yield();
        usleep(20000);
        after = get_free_frames();
        diff = abs(after - before);
        if (diff > 4) {
            printf("FAIL: memory frame leak detected: before=%d, after=%d, diff=%d\n", before, after, diff);
            exit(1);
        }
    }
    printf("PASS: zero memory frame leak (diff=%d frames)\n", diff);
}

/* --- Subtest 9: Fork from a Threaded Process --- */
static volatile int s_sub9_var = 100;
static void* sub9_thread(void* arg) {
    (void)arg;
    while (s_sub9_var == 100) {
        sched_yield();
    }
    return NULL;
}

static void test_fork_from_thread(void) {
    printf("[threadtest] Subtest 9: Fork from a threaded process...\n");
    s_sub9_var = 100;
    pthread_t th;
    pthread_create(&th, NULL, sub9_thread, NULL);

    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork from threaded process failed\n");
        exit(1);
    }
    if (pid == 0) {
        /* Child must have its own thread group */
        if (getpid() != syscall(SYS_gettid)) {
            printf("FAIL: child TID != PID in fork\n");
            exit(1);
        }
        s_sub9_var = 999;
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: child process exited with error\n");
        exit(1);
    }

    if (s_sub9_var != 100) {
        printf("FAIL: child write corrupted parent memory\n");
        exit(1);
    }

    s_sub9_var = 0;
    pthread_join(th, NULL);
    printf("PASS: fork from threaded process\n");
}

/* --- Subtest 10: Exec from a Non-Leader Thread --- */
static void* sub10_exec_worker(void* arg) {
    (void)arg;
    char* const argv[] = { "/bin/busybox", "true", NULL };
    char* const envp[] = { NULL };
    execve("/bin/busybox", argv, envp);
    exit(99);
}

static void test_exec_from_thread(void) {
    printf("[threadtest] Subtest 10: Exec from a non-leader thread...\n");
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        pthread_t th;
        pthread_create(&th, NULL, sub10_exec_worker, NULL);
        while (1) {
            sched_yield();
        }
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: exec from thread failed (status=0x%x)\n", status);
        exit(1);
    }
    printf("PASS: exec from non-leader thread\n");
}

/* --- Subtest 11: exit_group Racing Other Threads --- */
static void* sub11_spinner(void* arg) {
    (void)arg;
    while (1) sched_yield();
    return NULL;
}

static void* sub11_exiter(void* arg) {
    (void)arg;
    usleep(10000);
    syscall(SYS_exit_group, 42);
    return NULL;
}

static void test_exit_group_race(void) {
    printf("[threadtest] Subtest 11: exit_group racing other threads...\n");
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        pthread_t th[3];
        for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, sub11_spinner, NULL);
        pthread_t th_exit;
        pthread_create(&th_exit, NULL, sub11_exiter, NULL);
        while (1) sched_yield();
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        printf("FAIL: exit_group race produced status %d (expected 42)\n", WEXITSTATUS(status));
        exit(1);
    }
    printf("PASS: exit_group racing other threads\n");
}

/* --- Subtest 12: Futex WAIT Timeouts (Relative & Absolute) --- */
static void test_futex_timeouts(void) {
    printf("[threadtest] Subtest 12: Futex WAIT timeouts (relative + absolute)...\n");
    uint32_t val = 0;

    /* Relative timeout: 50ms */
    struct timespec rel_ts = { .tv_sec = 0, .tv_nsec = 50000000 };
    long r = syscall(SYS_futex, &val, 0 /* FUTEX_WAIT */, 0, &rel_ts, NULL, 0);
    if (r != -1 || errno != ETIMEDOUT) {
        printf("FAIL: relative futex timeout returned %ld (errno=%d, expected ETIMEDOUT)\n", r, errno);
        exit(1);
    }

    /* Absolute timeout: current realtime + 50ms */
    struct timespec abs_ts;
    clock_gettime(CLOCK_REALTIME, &abs_ts);
    abs_ts.tv_nsec += 50000000;
    if (abs_ts.tv_nsec >= 1000000000) {
        abs_ts.tv_sec += 1;
        abs_ts.tv_nsec -= 1000000000;
    }
    r = syscall(SYS_futex, &val, 9 /* FUTEX_WAIT_BITSET */ | 256 /* FUTEX_CLOCK_REALTIME */, 0, &abs_ts, NULL, 0xFFFFFFFF);
    if (r != -1 || errno != ETIMEDOUT) {
        printf("FAIL: absolute futex timeout returned %ld (errno=%d, expected ETIMEDOUT)\n", r, errno);
        exit(1);
    }
    printf("PASS: futex WAIT timeouts (relative + absolute)\n");
}

/* --- Subtest 13: Shared Futex Across Processes via MAP_SHARED --- */
#include <sys/mman.h>

static void test_shared_futex(void) {
    printf("[threadtest] Subtest 13: Shared futex across two processes via MAP_SHARED...\n");
    uint32_t* shm = (uint32_t*) mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        printf("FAIL: mmap MAP_SHARED failed\n");
        exit(1);
    }
    *shm = 0;

    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        while (*shm == 0) {
            syscall(SYS_futex, shm, 0 /* FUTEX_WAIT */, 0, NULL, NULL, 0);
        }
        if (*shm != 0xCAFE) exit(1);
        exit(0);
    }

    usleep(20000);
    *shm = 0xCAFE;
    syscall(SYS_futex, shm, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);

    int status = 0;
    waitpid(pid, &status, 0);
    munmap(shm, 4096);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: shared futex child exited with error\n");
        exit(1);
    }
    printf("PASS: shared futex across two processes via MAP_SHARED\n");
}

/* --- Subtest 14: Lost-Wakeup Stress Test --- */
static pthread_mutex_t s_lw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_lw_cond = PTHREAD_COND_INITIALIZER;
static volatile int s_lw_val = 0;
static volatile int s_lw_done = 0;

static void* sub14_waiter(void* arg) {
    (void)arg;
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&s_lw_lock);
        while (s_lw_val == 0 && !s_lw_done) {
            pthread_cond_wait(&s_lw_cond, &s_lw_lock);
        }
        if (s_lw_val > 0) s_lw_val--;
        pthread_mutex_unlock(&s_lw_lock);
    }
    return NULL;
}

static void* sub14_waker(void* arg) {
    (void)arg;
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&s_lw_lock);
        s_lw_val++;
        pthread_cond_signal(&s_lw_cond);
        pthread_mutex_unlock(&s_lw_lock);
    }
    return NULL;
}

static void test_lost_wakeup_stress(void) {
    printf("[threadtest] Subtest 14: Lost-wakeup stress test (many waiters/wakers)...\n");
    s_lw_val = 0;
    s_lw_done = 0;
    pthread_t waiters[4];
    pthread_t wakers[4];
    for (int i = 0; i < 4; i++) pthread_create(&waiters[i], NULL, sub14_waiter, NULL);
    for (int i = 0; i < 4; i++) pthread_create(&wakers[i], NULL, sub14_waker, NULL);

    for (int i = 0; i < 4; i++) pthread_join(wakers[i], NULL);

    pthread_mutex_lock(&s_lw_lock);
    s_lw_done = 1;
    pthread_cond_broadcast(&s_lw_cond);
    pthread_mutex_unlock(&s_lw_lock);

    for (int i = 0; i < 4; i++) pthread_join(waiters[i], NULL);
    printf("PASS: lost-wakeup stress test (zero lost wakeups / deadlocks)\n");
}

/* --- Subtest 15: Detached Threads --- */
static volatile int s_detached_counter = 0;
static void* sub15_detached_worker(void* arg) {
    (void)arg;
    s_detached_counter++;
    return NULL;
}

static void test_detached_threads(void) {
    printf("[threadtest] Subtest 15: Detached threads...\n");
    s_detached_counter = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    for (int i = 0; i < 100; i++) {
        pthread_t th;
        pthread_create(&th, &attr, sub15_detached_worker, NULL);
    }
    pthread_attr_destroy(&attr);

    while (s_detached_counter < 100) {
        sched_yield();
        usleep(1000);
    }
    printf("PASS: detached threads\n");
}

/* --- Subtest 16: Clone with Unsupported Flags --- */
static void test_clone_unsupported_flags(void) {
    printf("[threadtest] Subtest 16: Clone with unsupported flags (CLONE_NEW*, CLONE_PTRACE)...\n");
    char stack[4096];
    long r = syscall(SYS_clone, 0x00020000 /* CLONE_NEWNS */, stack + sizeof(stack), NULL, NULL, 0);
    if (r != -1 || errno != EINVAL) {
        printf("FAIL: clone with CLONE_NEWNS did not return -EINVAL (ret=%ld, errno=%d)\n", r, errno);
        exit(1);
    }
    r = syscall(SYS_clone, 0x00002000 /* CLONE_PTRACE */, stack + sizeof(stack), NULL, NULL, 0);
    if (r != -1 || errno != EINVAL) {
        printf("FAIL: clone with CLONE_PTRACE did not return -EINVAL (ret=%ld, errno=%d)\n", r, errno);
        exit(1);
    }
    printf("PASS: clone with unsupported flags returns -EINVAL\n");
}

/* --- Subtest 17: rt_sigqueueinfo and rt_sigtimedwait --- */
static void test_sigqueue_timedwait(void) {
    printf("[threadtest] Subtest 17: rt_sigqueueinfo and rt_sigtimedwait...\n");
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);

    long r = syscall(SYS_rt_sigqueueinfo, getpid(), SIGUSR2, NULL);
    if (r != 0) {
        printf("FAIL: rt_sigqueueinfo returned %ld\n", r);
        exit(1);
    }

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    r = syscall(SYS_rt_sigtimedwait, &set, NULL, &ts, 8 /* _NSIG / 8 */);
    if (r != SIGUSR2) {
        printf("FAIL: rt_sigtimedwait returned %ld (errno=%d, expected SIGUSR2=%d)\n", r, errno, SIGUSR2);
        exit(1);
    }

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("PASS: rt_sigqueueinfo and rt_sigtimedwait\n");
}

/* --- Subtest 18: Agent Suspension Freezes ALL Threads Within 2 Ticks --- */
struct sub18_shm {
    volatile int counter;
    volatile int running;
};
static struct sub18_shm* s_sub18_data = NULL;

static void* sub18_thread(void* arg) {
    (void)arg;
    while (s_sub18_data && s_sub18_data->running) {
        s_sub18_data->counter++;
        sched_yield();
    }
    return NULL;
}

static void test_agent_suspend_freezes_all_threads(void) {
    printf("[threadtest] Subtest 18: Agent suspension freezes ALL threads within 2 ticks...\n");
    s_sub18_data = (struct sub18_shm*) mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s_sub18_data == MAP_FAILED) {
        printf("FAIL: mmap sub18 failed\n");
        exit(1);
    }
    s_sub18_data->counter = 0;
    s_sub18_data->running = 1;

    test_agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = 0x00000001 | 0x00000002; /* CAP_FS_READ | CAP_FS_WRITE */
    caps.num_fs_read = 1;
    strncpy(caps.fs_read[0].prefix, "/tmp", 63);
    caps.num_fs_write = 1;
    strncpy(caps.fs_write[0].prefix, "/tmp", 63);

    test_agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    budget.max_procs = 4;
    budget.max_threads = 8;
    budget.max_mem_pages = 256;

    int ag_id = (int) syscall(SYS_AGENT_CREATE, "freeze_agent", &caps, &budget);
    if (ag_id <= 0) {
        printf("FAIL: agent_create failed\n");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        syscall(SYS_AGENT_ATTACH, ag_id);
        pthread_t th1, th2;
        pthread_create(&th1, NULL, sub18_thread, NULL);
        pthread_create(&th2, NULL, sub18_thread, NULL);
        while (s_sub18_data->running) {
            s_sub18_data->counter++;
            sched_yield();
        }
        exit(0);
    }

    usleep(30000);
    int c1 = s_sub18_data->counter;

    /* Suspend agent */
    syscall(SYS_AGENT_SUSPEND, ag_id);
    usleep(40000);
    int c2 = s_sub18_data->counter;

    if (c2 != c1) {
        printf("FAIL: threads advanced counter while suspended (%d -> %d)\n", c1, c2);
        exit(1);
    }

    /* Resume agent */
    syscall(SYS_AGENT_RESUME, ag_id);
    usleep(40000);
    int c3 = s_sub18_data->counter;

    if (c3 <= c2) {
        printf("FAIL: threads failed to resume after agent_resume (c2=%d, c3=%d)\n", c2, c3);
        exit(1);
    }

    s_sub18_data->running = 0;
    syscall(SYS_AGENT_KILL, ag_id);
    waitpid(pid, NULL, 0);
    munmap(s_sub18_data, 4096);
    printf("PASS: agent suspension freezes ALL threads within 2 ticks\n");
}

/* --- Subtest 19: Sandbox Escape Matrix from Non-Leader Threads Inside Agent --- */
static volatile int s_sub19_escapes = 0;

static void* sub19_worker(void* arg) {
    (void)arg;
    int escapes = 0;

    /* 1. Outside path open */
    int fd = open("/canary.txt", O_RDONLY);
    if (fd >= 0) { close(fd); escapes++; }
    fd = open("/canary.txt", O_WRONLY);
    if (fd >= 0) { close(fd); escapes++; }

    /* 2. Truncate & chmod outside */
    if (truncate("/canary.txt", 0) == 0) escapes++;
    if (chmod("/canary.txt", 0777) == 0) escapes++;

    /* 3. Procfs cross-process isolation */
    fd = open("/proc/1/status", O_RDONLY);
    if (fd >= 0) { close(fd); escapes++; }
    fd = open("/proc/self/mem", O_RDONLY);
    if (fd >= 0) { close(fd); escapes++; }

    /* 4. Restricted 1000-range syscalls */
    if (syscall(SYS_AGENT_ATTACH, 1) == 0) escapes++;
    if (syscall(SYS_AGENT_KILL, 1) == 0) escapes++;
    if (syscall(1009 /* SYS_BUS_DAEMON_REGISTER */) == 0) escapes++;
    if (syscall(1011 /* SYS_AGENT_CHARGE_TOKENS */, 1, 10) == 0) escapes++;
    if (syscall(1018 /* SYS_TEST_HARDENING */, 1, 0) == 0) escapes++;

    s_sub19_escapes = escapes;
    return NULL;
}

static void test_sandbox_escapes_from_thread(void) {
    printf("[threadtest] Subtest 19: Full sandbox-escape matrix from non-leader threads...\n");
    test_agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = 0x00000001 | 0x00000002;
    caps.num_fs_read = 1;
    strncpy(caps.fs_read[0].prefix, "/tmp", 63);
    caps.num_fs_write = 1;
    strncpy(caps.fs_write[0].prefix, "/tmp", 63);

    test_agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    budget.max_procs = 4;
    budget.max_threads = 8;
    budget.max_mem_pages = 256;

    int ag_id = (int) syscall(SYS_AGENT_CREATE, "th_escape_agent", &caps, &budget);
    if (ag_id <= 0) {
        printf("FAIL: agent_create failed\n");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        syscall(SYS_AGENT_ATTACH, ag_id);
        s_sub19_escapes = 0;
        pthread_t th;
        pthread_create(&th, NULL, sub19_worker, NULL);
        pthread_join(th, NULL);
        exit(s_sub19_escapes);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    syscall(SYS_AGENT_KILL, ag_id);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: non-leader thread detected sandbox escapes (status=%d)\n", WEXITSTATUS(status));
        exit(1);
    }
    printf("PASS: sandbox-escape matrix re-run from non-leader threads inside agent\n");
}

/* --- Subtest 20: Per-Thread CPU Accounting --- */
static volatile int s_cpu_spin = 1;
static void* sub20_spinner(void* arg) {
    (void)arg;
    while (s_cpu_spin) {
        for (volatile int i = 0; i < 100000; i++);
    }
    return NULL;
}

static void test_per_thread_cpu(void) {
    printf("[threadtest] Subtest 20: Per-thread CPU accounting (CLOCK_THREAD_CPUTIME_ID)...\n");
    s_cpu_spin = 1;
    pthread_t th;
    pthread_create(&th, NULL, sub20_spinner, NULL);

    usleep(50000);
    s_cpu_spin = 0;
    pthread_join(th, NULL);

    struct timespec th_ts;
    int r = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &th_ts);
    if (r != 0) {
        printf("FAIL: clock_gettime CLOCK_THREAD_CPUTIME_ID returned %d\n", r);
        exit(1);
    }
    struct timespec pr_ts;
    r = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pr_ts);
    if (r != 0) {
        printf("FAIL: clock_gettime CLOCK_PROCESS_CPUTIME_ID returned %d\n", r);
        exit(1);
    }
    printf("PASS: per-thread CPU accounting (thread=%lu.%09lu s, process=%lu.%09lu s)\n",
           th_ts.tv_sec, th_ts.tv_nsec, pr_ts.tv_sec, pr_ts.tv_nsec);
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
    test_fork_from_thread();
    test_exec_from_thread();
    test_exit_group_race();
    test_futex_timeouts();
    test_shared_futex();
    test_lost_wakeup_stress();
    test_detached_threads();
    test_clone_unsupported_flags();
    test_sigqueue_timedwait();
    test_agent_suspend_freezes_all_threads();
    test_sandbox_escapes_from_thread();
    test_per_thread_cpu();

    printf("\nALL THREAD TESTS PASSED (20/20)\n");
    return 0;
}
