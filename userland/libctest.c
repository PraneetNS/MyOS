/*
 * userland/libctest.c - Exhaustive verification suite for Newlib libc & POSIX emulation
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>

#define ASSERT(expr, msg) do { \
    if (!(expr)) { \
        printf("[FAIL] %s: %s (line %d)\n", __func__, msg, __LINE__); \
        exit(1); \
    } \
} while (0)

/* 1. Format string test: printf, sprintf, snprintf */
static void test_printf_formats(void) {
    char buf[256];
    int n;

    // %d, %u, %x, %o, %c, %s, %p, %lld
    n = snprintf(buf, sizeof(buf), "%d %u %x %o %c %s %p %lld",
                 -42, 42u, 0x2a, 052, 'Z', "libc", (void*)0x1234, 1234567890123LL);
    ASSERT(n > 0, "snprintf format integer/string");
    ASSERT(strstr(buf, "-42 42 2a 52 Z libc") != NULL, "integer format mismatch");

    // %f, %e, %g, %5.2f, %-8s
    n = snprintf(buf, sizeof(buf), "%.2f %5.2f %-8s", 3.14159, 2.5, "align");
    ASSERT(n > 0, "snprintf float/width");
    ASSERT(strstr(buf, "3.14  2.50 align   ") != NULL, "float/width mismatch");

    printf("[libctest] test_printf_formats OK\n");
}

/* 2. sscanf test */
static void test_sscanf(void) {
    const char *input = "123 0x7f 3.14 hello";
    int a = 0, b = 0;
    float c = 0.0f;
    char str[32] = {0};

    int count = sscanf(input, "%d 0x%x %f %s", &a, &b, &c, str);
    ASSERT(count == 4, "sscanf item count");
    ASSERT(a == 123, "sscanf int");
    ASSERT(b == 0x7f, "sscanf hex");
    ASSERT(fabsf(c - 3.14f) < 0.01f, "sscanf float");
    ASSERT(strcmp(str, "hello") == 0, "sscanf string");

    printf("[libctest] test_sscanf OK\n");
}

/* 3. stdio file operations */
static void test_stdio_files(void) {
    const char *path = "/tmp/f_orig.txt";
    const char *renamed = "/tmp/f_ren.txt";

    // fopen w, fputs, fwrite, fclose
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL, "fopen w");
    ASSERT(fputs("Line one\n", fp) >= 0, "fputs");
    const char *bin_data = "ABCD1234\n";
    size_t written = fwrite(bin_data, 1, strlen(bin_data), fp);
    ASSERT(written == strlen(bin_data), "fwrite");
    fflush(fp);
    fclose(fp);

    // fopen r, fgets, fread, fseek, ftell, rewind
    fp = fopen(path, "r");
    ASSERT(fp != NULL, "fopen r");
    char line[64] = {0};
    ASSERT(fgets(line, sizeof(line), fp) != NULL, "fgets");
    ASSERT(strcmp(line, "Line one\n") == 0, "fgets content");

    long pos = ftell(fp);
    ASSERT(pos == 9, "ftell");

    ASSERT(fseek(fp, 0, SEEK_END) == 0, "fseek END");
    long end_pos = ftell(fp);
    ASSERT(end_pos == 9 + (long)strlen(bin_data), "ftell end");

    rewind(fp);
    ASSERT(ftell(fp) == 0, "rewind");

    char buf[32] = {0};
    size_t r = fread(buf, 1, 4, fp);
    ASSERT(r == 4, "fread");
    ASSERT(memcmp(buf, "Line", 4) == 0, "fread content");
    fclose(fp);

    // rename and remove
    remove(renamed);
    ASSERT(rename(path, renamed) == 0, "rename");
    ASSERT(fopen(path, "r") == NULL, "old path gone");
    FILE *rfp = fopen(renamed, "r");
    ASSERT(rfp != NULL, "new path exists");
    fclose(rfp);
    ASSERT(remove(renamed) == 0, "remove");

    // tmpfile
    FILE *tf = tmpfile();
    if (tf) {
        fputs("temp test", tf);
        rewind(tf);
        char tbuf[16] = {0};
        fgets(tbuf, sizeof(tbuf), tf);
        ASSERT(strcmp(tbuf, "temp test") == 0, "tmpfile content");
        fclose(tf);
    }

    printf("[libctest] test_stdio_files OK\n");
}

/* 4. Malloc/calloc/realloc/free stress */
static void test_heap_memory(void) {
    #define NUM_BLOCKS 64
    #define BLOCK_SIZE 1024
    void *ptrs[NUM_BLOCKS];

    // calloc check (must be all zeroes)
    for (int i = 0; i < NUM_BLOCKS; i++) {
        ptrs[i] = calloc(1, BLOCK_SIZE);
        ASSERT(ptrs[i] != NULL, "calloc");
        unsigned char *p = (unsigned char *)ptrs[i];
        for (int j = 0; j < BLOCK_SIZE; j++) {
            ASSERT(p[j] == 0, "calloc zero");
            p[j] = (unsigned char)(i ^ j);
        }
    }

    // verify patterns and realloc
    for (int i = 0; i < NUM_BLOCKS; i++) {
        unsigned char *p = (unsigned char *)ptrs[i];
        for (int j = 0; j < BLOCK_SIZE; j++) {
            ASSERT(p[j] == (unsigned char)(i ^ j), "pattern check");
        }
        ptrs[i] = realloc(ptrs[i], BLOCK_SIZE * 2);
        ASSERT(ptrs[i] != NULL, "realloc expand");
        p = (unsigned char *)ptrs[i];
        for (int j = 0; j < BLOCK_SIZE; j++) {
            ASSERT(p[j] == (unsigned char)(i ^ j), "pattern preserved after realloc");
        }
    }

    // free all
    for (int i = 0; i < NUM_BLOCKS; i++) {
        free(ptrs[i]);
    }

    printf("[libctest] test_heap_memory OK\n");
}

/* 5. Algorithms: qsort, bsearch, strtol, strtod, atof */
static int cmp_int(const void *a, const void *b) {
    return (*(const int*)a - *(const int*)b);
}

static void test_stdlib_algorithms(void) {
    int arr[] = { 42, 17, 99, -5, 0, 1000, 3 };
    int n = sizeof(arr) / sizeof(arr[0]);
    qsort(arr, n, sizeof(int), cmp_int);

    for (int i = 0; i < n - 1; i++) {
        ASSERT(arr[i] <= arr[i+1], "qsort ordering");
    }

    int key = 42;
    int *found = (int*)bsearch(&key, arr, n, sizeof(int), cmp_int);
    ASSERT(found != NULL && *found == 42, "bsearch found");

    int missing = 999;
    ASSERT(bsearch(&missing, arr, n, sizeof(int), cmp_int) == NULL, "bsearch missing");

    // strtol, strtod, atof
    char *endptr;
    long l = strtol("  -12345abc", &endptr, 10);
    ASSERT(l == -12345, "strtol decimal");
    ASSERT(strcmp(endptr, "abc") == 0, "strtol endptr");

    long hex = strtol("0x7EADBEEF", NULL, 16);
    ASSERT(hex == 0x7EADBEEF, "strtol hex");

    unsigned long uhex = strtoul("0xDEADBEEF", NULL, 16);
    ASSERT(uhex == 0xDEADBEEFUL, "strtoul hex");

    double d = strtod("3.14159265extra", &endptr);
    ASSERT(fabs(d - 3.14159265) < 1e-6, "strtod");
    ASSERT(strcmp(endptr, "extra") == 0, "strtod endptr");

    double d2 = atof("-0.0075");
    ASSERT(fabs(d2 - (-0.0075)) < 1e-6, "atof");

    printf("[libctest] test_stdlib_algorithms OK\n");
}

/* 6. string.h and ctype.h */
static void test_string_and_ctype(void) {
    char s1[32] = "Hello, World!";
    char s2[32];
    ASSERT(strlen(s1) == 13, "strlen");
    strcpy(s2, s1);
    ASSERT(strcmp(s1, s2) == 0, "strcmp");
    ASSERT(strncmp(s1, "Hello, There", 7) == 0, "strncmp");
    ASSERT(strchr(s1, 'W') == s1 + 7, "strchr");
    ASSERT(strstr(s1, "World") == s1 + 7, "strstr");

    memmove(s1 + 7, s1, 5); // overlap copy: "Hello, Hello!"
    ASSERT(strncmp(s1 + 7, "Hello", 5) == 0, "memmove");

    ASSERT(isalpha('a') && isalpha('Z') && !isalpha('1'), "isalpha");
    ASSERT(isdigit('0') && isdigit('9') && !isdigit('a'), "isdigit");
    ASSERT(isspace(' ') && isspace('\n') && !isspace('x'), "isspace");
    ASSERT(toupper('a') == 'A' && tolower('Z') == 'z', "toupper/tolower");

    printf("[libctest] test_string_and_ctype OK\n");
}

/* 7. setjmp / longjmp */
static void test_setjmp_longjmp(void) {
    jmp_buf env;
    volatile int counter = 0;

    int r = setjmp(env);
    if (r == 0) {
        counter++;
        longjmp(env, 42);
        ASSERT(0, "unreachable after longjmp");
    } else {
        ASSERT(r == 42, "longjmp return value");
        ASSERT(counter == 1, "counter preserved across longjmp");
    }

    printf("[libctest] test_setjmp_longjmp OK\n");
}

/* 8. getenv / setenv */
static void test_env_vars(void) {
    setenv("LIBCTEST_VAR", "my_value_123", 1);
    char *val = getenv("LIBCTEST_VAR");
    ASSERT(val != NULL && strcmp(val, "my_value_123") == 0, "getenv matched setenv");

    setenv("LIBCTEST_VAR", "overwritten", 1);
    val = getenv("LIBCTEST_VAR");
    ASSERT(val != NULL && strcmp(val, "overwritten") == 0, "setenv overwrite");

    printf("[libctest] test_env_vars OK\n");
}

/* 9. time / strftime / localtime */
static void test_time_and_date(void) {
    time_t now = time(NULL);
    ASSERT(now > 1700000000, "time_t plausible unix timestamp (>= 2023)");

    struct tm *lt = localtime(&now);
    ASSERT(lt != NULL, "localtime");
    ASSERT(lt->tm_year + 1900 >= 2026, "year >= 2026");

    char tbuf[64] = {0};
    size_t s = strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", lt);
    ASSERT(s > 0, "strftime");
    ASSERT(strncmp(tbuf, "202", 3) == 0, "strftime year starts with 202");

    printf("[libctest] test_time_and_date OK\n");
}

/* 10. Math functions: sin, cos, tan, sqrt, exp, log, pow, floor, ceil, fmod */
static void test_math_functions(void) {
    const double eps = 1e-4;

    // sin, cos, tan
    double s = sin(0.0);
    ASSERT(fabs(s - 0.0) < eps, "sin(0)");
    double c = cos(0.0);
    ASSERT(fabs(c - 1.0) < eps, "cos(0)");
    double t = tan(M_PI / 4.0);
    ASSERT(fabs(t - 1.0) < eps, "tan(pi/4)");

    // sqrt
    double sq = sqrt(144.0);
    ASSERT(fabs(sq - 12.0) < eps, "sqrt(144)");
    float sqf = sqrtf(25.0f);
    ASSERT(fabsf(sqf - 5.0f) < (float)eps, "sqrtf(25)");

    // exp, log, pow
    double e = exp(1.0);
    ASSERT(fabs(e - 2.71828) < 1e-3, "exp(1)");
    double l = log(e);
    ASSERT(fabs(l - 1.0) < eps, "log(e)");
    double p = pow(2.0, 10.0);
    ASSERT(fabs(p - 1024.0) < eps, "pow(2, 10)");

    // floor, ceil, fmod
    ASSERT(floor(3.7) == 3.0, "floor");
    ASSERT(ceil(3.2) == 4.0, "ceil");
    double m = fmod(5.5, 2.0);
    ASSERT(fabs(m - 1.5) < eps, "fmod");

    printf("[libctest] test_math_functions OK\n");
}

/* 11. errno checks for failing syscalls */
static void test_syscall_errno(void) {
    errno = 0;
    int fd = open("/nonexistent_path_xyz_12345", O_RDONLY);
    ASSERT(fd < 0, "open non-existent file failed");
    ASSERT(errno == ENOENT, "errno is ENOENT");

    errno = 0;
    int r = close(-1);
    ASSERT(r < 0, "close invalid fd failed");
    ASSERT(errno == EBADF, "errno is EBADF");

    printf("[libctest] test_syscall_errno OK\n");
}

/* 12. opendir / readdir / closedir */
static void test_directory_listing(void) {
    DIR *dir = opendir("/bin");
    ASSERT(dir != NULL, "opendir /bin");

    int found_sh = 0, found_lua = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, "sh.elf") == 0 || strcmp(de->d_name, "sh") == 0) found_sh = 1;
        if (strcmp(de->d_name, "lua.elf") == 0 || strcmp(de->d_name, "lua") == 0) found_lua = 1;
    }
    closedir(dir);

    ASSERT(found_sh, "found sh in /bin");
    ASSERT(found_lua, "found lua in /bin");

    printf("[libctest] test_directory_listing OK\n");
}

/* 13. fork + pipe + waitpid */
static void test_fork_pipe(void) {
    int pfd[2];
    ASSERT(pipe(pfd) == 0, "pipe");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");

    if (pid == 0) {
        // Child
        close(pfd[0]);
        const char *msg = "PIPE_TEST_MSG";
        write(pfd[1], msg, strlen(msg));
        close(pfd[1]);
        exit(0);
    } else {
        // Parent
        close(pfd[1]);
        char buf[32] = {0};
        int n = read(pfd[0], buf, sizeof(buf) - 1);
        ASSERT(n == 13, "read bytes from pipe");
        ASSERT(strcmp(buf, "PIPE_TEST_MSG") == 0, "pipe msg matched");
        close(pfd[0]);

        int status = 0;
        pid_t waited = waitpid(pid, &status, 0);
        ASSERT(waited == pid, "waitpid");
        ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit 0");
    }

    printf("[libctest] test_fork_pipe OK\n");
}

/* 14. sigaction handler */
static volatile int sig_handled = 0;
static void my_handler(int sig) {
    if (sig == SIGUSR1) sig_handled = 1;
}

static void test_sigaction(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = my_handler;
    ASSERT(sigaction(SIGUSR1, &sa, NULL) == 0, "sigaction");

    raise(SIGUSR1);
    ASSERT(sig_handled == 1, "signal handler executed");

    printf("[libctest] test_sigaction OK\n");
}

/* 15. mmap anonymous */
static void test_mmap_anon(void) {
    size_t sz = 4096;
    void *ptr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(ptr != MAP_FAILED && ptr != NULL, "mmap anonymous");

    char *buf = (char *)ptr;
    strcpy(buf, "MMAP_TEST_PAYLOAD");
    ASSERT(strcmp(buf, "MMAP_TEST_PAYLOAD") == 0, "mmap write and read");

    ASSERT(munmap(ptr, sz) == 0, "munmap");

    printf("[libctest] test_mmap_anon OK\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== Starting Newlib libc test suite (libctest) ===\n");

    test_printf_formats();
    test_sscanf();
    test_stdio_files();
    test_heap_memory();
    test_stdlib_algorithms();
    test_string_and_ctype();
    test_setjmp_longjmp();
    test_env_vars();
    test_time_and_date();
    test_math_functions();
    test_syscall_errno();
    test_directory_listing();
    test_fork_pipe();
    test_sigaction();
    test_mmap_anon();

    printf("[libctest] ALL TESTS PASSED\n");
    return 0;
}
