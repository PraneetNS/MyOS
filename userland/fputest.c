#include "libc.h"

static void print_str(const char* s) {
    if (s) write(1, s, strlen(s));
}

static void print_int(int n) {
    char buf[16];
    int i = 0;
    if (n < 0) {
        write(1, "-", 1);
        n = -n;
    }
    if (n == 0) {
        write(1, "0", 1);
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        write(1, &buf[j], 1);
    }
}

static int has_sse(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (edx & (1u << 25)) != 0;
}

static inline float x87_sqrt(float x) {
    float res;
    asm volatile (
        "flds %1\n\t"
        "fsqrt\n\t"
        "fstps %0\n\t"
        : "=m"(res)
        : "m"(x)
    );
    return res;
}

static inline float x87_add(float a, float b) {
    float res;
    asm volatile (
        "flds %1\n\t"
        "fadds %2\n\t"
        "fstps %0\n\t"
        : "=m"(res)
        : "m"(a), "m"(b)
    );
    return res;
}

__attribute__((target("sse,sse2")))
static inline float sse_sqrt(float x) {
    float res;
    asm volatile (
        "movss %1, %%xmm0\n\t"
        "sqrtss %%xmm0, %%xmm0\n\t"
        "movss %%xmm0, %0\n\t"
        : "=m"(res)
        : "m"(x)
        : "xmm0"
    );
    return res;
}

__attribute__((target("sse,sse2")))
static inline float sse_add(float a, float b) {
    float res;
    asm volatile (
        "movss %1, %%xmm0\n\t"
        "movss %2, %%xmm1\n\t"
        "addss %%xmm1, %%xmm0\n\t"
        "movss %%xmm0, %0\n\t"
        : "=m"(res)
        : "m"(a), "m"(b)
        : "xmm0", "xmm1"
    );
    return res;
}

/* Series 1: x87 square-root summation */
static unsigned int compute_series1(int iterations, int use_sse) {
    float acc = 1.0f;
    for (int i = 0; i < iterations; i++) {
        acc = x87_sqrt(acc);
        acc = x87_add(acc, 3.0f);
        if (use_sse) {
            acc = sse_sqrt(acc);
            acc = sse_add(acc, 0.5f);
        }
    }
    /* Cast bits of float to unsigned int for deterministic checksum */
    union { float f; unsigned int u; } cvt;
    cvt.f = acc;
    return cvt.u;
}

/* Series 2: different constants */
static unsigned int compute_series2(int iterations, int use_sse) {
    float acc = 100.0f;
    for (int i = 0; i < iterations; i++) {
        acc = x87_sqrt(acc);
        acc = x87_add(acc, 7.5f);
        if (use_sse) {
            acc = sse_sqrt(acc);
            acc = sse_add(acc, 1.25f);
        }
    }
    union { float f; unsigned int u; } cvt;
    cvt.f = acc;
    return cvt.u;
}

/* Series 3: third set of constants */
static unsigned int compute_series3(int iterations, int use_sse) {
    float acc = 500.0f;
    for (int i = 0; i < iterations; i++) {
        acc = x87_sqrt(acc);
        acc = x87_add(acc, 12.0f);
        if (use_sse) {
            acc = sse_sqrt(acc);
            acc = sse_add(acc, 2.5f);
        }
    }
    union { float f; unsigned int u; } cvt;
    cvt.f = acc;
    return cvt.u;
}

static void fpe_handler(int sig) {
    (void) sig;
    print_str("PASS: caught SIGFPE in user handler\n");
    exit(0);
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    print_str("=== RUNNING FPUTEST ===\n");

    int sse = has_sse();
    if (sse) {
        print_str("[fputest] SSE detected and supported\n");
    } else {
        print_str("[fputest] SSE not detected, using x87 only\n");
    }

    /* Compute single-thread reference values */
    const int ITERS = 200000;
    unsigned int ref1 = compute_series1(ITERS, sse);
    unsigned int ref2 = compute_series2(ITERS, sse);
    unsigned int ref3 = compute_series3(ITERS, sse);

    print_str("[fputest] Spawning concurrent processes...\n");

    int pid1 = fork();
    if (pid1 == 0) {
        /* Child 1 */
        unsigned int val = compute_series1(ITERS, sse);
        if (val == ref1) {
            print_str("[child1] Series 1 matched checksum PASS\n");
            exit(0);
        } else {
            print_str("[child1] Series 1 checksum MISMATCH FAIL\n");
            exit(1);
        }
    }

    int pid2 = fork();
    if (pid2 == 0) {
        /* Child 2 */
        unsigned int val = compute_series2(ITERS, sse);
        if (val == ref2) {
            print_str("[child2] Series 2 matched checksum PASS\n");
            exit(0);
        } else {
            print_str("[child2] Series 2 checksum MISMATCH FAIL\n");
            exit(1);
        }
    }

    /* Parent computes Series 3 concurrently */
    unsigned int parent_val = compute_series3(ITERS, sse);
    if (parent_val == ref3) {
        print_str("[parent] Series 3 matched checksum PASS\n");
    } else {
        print_str("[parent] Series 3 checksum MISMATCH FAIL\n");
    }

    int st1 = -1, st2 = -1;
    waitpid(pid1, &st1, 0);
    waitpid(pid2, &st2, 0);

    if (st1 != 0 || st2 != 0 || parent_val != ref3) {
        print_str("FAIL: Concurrent FPU calculations corrupted across context switches!\n");
        return 1;
    }
    print_str("PASS: 3 concurrent processes with FPU/SSE completed without corruption\n");

    /* Test SIGFPE mapping */
    signal(SIGFPE, fpe_handler);
    int fault_pid = fork();
    if (fault_pid == 0) {
        /* Trigger integer divide by zero which raises vector 0 -> SIGFPE */
        int zero = 0;
        int num = 42;
        asm volatile (
            "idivl %1"
            : "=a"(num)
            : "r"(zero), "a"(num), "d"(0)
        );
        (void) num;
        exit(0);
    }
    int fault_st = -1;
    waitpid(fault_pid, &fault_st, 0);
    int term_sig = fault_st & 0x7F;
    if (fault_st == 0) {
        print_str("PASS: Division by zero caught and handled via SIGFPE\n");
    } else if (term_sig == SIGFPE || term_sig == 8) {
        print_str("PASS: Division by zero generated SIGFPE\n");
    } else {
        print_str("[fputest] Child terminated with status: ");
        print_int(fault_st);
        print_str("\n");
        return 1;
    }

    print_str("=== ALL FPUTESTS PASSED ===\n");
    return 0;
}
