#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>

static volatile int sig1_received = 0;
static volatile int sig2_received = 0;

static void handler2(int sig) {
    (void)sig;
    sig2_received = 1;
}

static void handler1(int sig) {
    (void)sig;
    sig1_received = 1;
    raise(SIGUSR2);
}

static int read_kstack_hwm(void) {
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return -1;
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char* p = strstr(buf, "kstack_hwm:\t");
    if (!p) {
        p = strstr(buf, "kstack_hwm: ");
        if (!p) return -1;
        p += 12;
    } else {
        p += 12;
    }
    return atoi(p);
}

int main(void) {
    printf("[stacktest] 1. Deep path test: FAT LFN create and rename...\n");
    const char* f1 = "/lfn_depth_test_file_alpha_beta_gamma_123456789.txt";
    const char* f2 = "/lfn_depth_test_file_renamed_zeta_theta_987654321.txt";
    int fd = open(f1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "test", 4);
        close(fd);
        rename(f1, f2);
        unlink(f2);
    }

    printf("[stacktest] 2. Nested signal delivery test...\n");
    signal(SIGUSR1, handler1);
    signal(SIGUSR2, handler2);
    raise(SIGUSR1);
    printf("[stacktest] Signals received: sig1=%d sig2=%d\n", sig1_received, sig2_received);

    printf("[stacktest] 3. Socket IPC test...\n");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        write(sv[0], "ping", 4);
        char sbuf[16];
        read(sv[1], sbuf, 4);
        close(sv[0]);
        close(sv[1]);
    }

    int hwm = read_kstack_hwm();
    printf("[stacktest] Current kstack_hwm: %d bytes (limit: 32768 / 50%% of 64KB)\n", hwm);
    if (hwm > 0 && hwm < 32768) {
        printf("PASS: kstack_hwm under 50%% threshold\n");
    } else {
        printf("FAIL: kstack_hwm exceeded 50%% limit (hwm=%d)\n", hwm);
        return 1;
    }

    printf("[stacktest] 4. Testing kernel stack guard page overflow protection...\n");
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: invoke recursion syscall 1099 */
        printf("[stacktest] Child PID %d calling recursion syscall 1099...\n", getpid());
        long ret;
        asm volatile (
            "mov $1099, %%rax\n\t"
            "syscall\n\t"
            "mov %%rax, %0\n\t"
            : "=r"(ret)
            :
            : "rax", "rcx", "r11", "memory"
        );
        printf("FAIL: child survived stack overflow syscall (ret=%ld)\n", ret);
        exit(0);
    }

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    printf("[stacktest] waitpid returned %d, status=0x%x (WIFEXITED=%d exit=%d WIFSIGNALED=%d sig=%d)\n",
           w, status, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status), WTERMSIG(status));

    if (WIFEXITED(status) && WEXITSTATUS(status) == 139) {
        printf("PASS: child terminated with exit code 139 (stack guard fault caught)\n");
        return 0;
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == 11) {
        printf("PASS: child terminated with SIGSEGV (stack guard fault caught)\n");
        return 0;
    } else {
        printf("FAIL: unexpected child termination status\n");
        return 1;
    }
}
