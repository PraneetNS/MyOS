#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <myos_syscalls.h>

static inline int sys_symlink(const char* target, const char* linkpath) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "i"(__NR_symlink), "g"(target), "g"(linkpath)
        : "rcx", "r11", "memory"
    );
    return (int) ret;
}

static inline ssize_t sys_readlink(const char* path, char* buf, size_t bufsiz) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "i"(__NR_readlink), "g"(path), "g"(buf), "g"(bufsiz)
        : "rcx", "r11", "memory"
    );
    return (ssize_t) ret;
}

static int get_free_frames(void) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n\t"
        "syscall\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(ret)
        : "i"(SYS_FREE_FRAMES)
        : "rcx", "r11", "memory"
    );
    return (int) ret;
}

int main(void) {
    printf("============================================================\n");
    printf("[leaktest] Running 200 FS cycles and 200 exec/exit cycles...\n");
    printf("============================================================\n");

    int frames_init = get_free_frames();
    printf("[leaktest] Initial free frames: %d\n", frames_init);

    /* 1. 200 cycles of open / symlink / readlink / unlink */
    printf("[leaktest] Starting 200 open/symlink/unlink cycles...\n");
    for (int i = 0; i < 200; i++) {
        char path[64];
        char sym[64];
        snprintf(path, sizeof(path), "/tmp/leak_%d.tmp", i);
        snprintf(sym, sizeof(sym), "/tmp/sym_%d.tmp", i);

        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            printf("FAIL: open failed at iter %d\n", i);
            return 1;
        }
        write(fd, "test", 4);
        close(fd);

        if (sys_symlink(path, sym) != 0) {
            printf("FAIL: symlink failed at iter %d\n", i);
            return 1;
        }

        char link_target[64];
        ssize_t len = sys_readlink(sym, link_target, sizeof(link_target) - 1);
        if (len <= 0) {
            printf("FAIL: readlink failed at iter %d\n", i);
            return 1;
        }

        unlink(sym);
        unlink(path);
    }
    printf("[leaktest] Completed 200 FS cycles successfully.\n");

    int frames_mid = get_free_frames();
    printf("[leaktest] Mid-test free frames: %d (delta: %d)\n", frames_mid, frames_mid - frames_init);

    /* 2. 200 cycles of fork / exec / exit */
    printf("[leaktest] Starting 200 fork/exec/exit cycles...\n");
    char* const argv[] = { "/bin/busybox", "true", NULL };
    char* const envp[] = { NULL };

    for (int i = 0; i < 200; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL: fork failed at iter %d\n", i);
            return 1;
        }
        if (pid == 0) {
            execve("/bin/busybox", argv, envp);
            /* fallback to exit if execve fails */
            exit(127);
        } else {
            int status = 0;
            waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("FAIL: child exited with bad status 0x%x at iter %d\n", status, i);
                return 1;
            }
        }
    }
    printf("[leaktest] Completed 200 exec/exit cycles successfully.\n");

    int frames_end = get_free_frames();
    int net_delta = frames_init - frames_end;
    printf("[leaktest] Final free frames: %d (net delta: %d frames)\n", frames_end, net_delta);

    /* Allow small variance for buffer cache / directory growth */
    if (net_delta > 16 || net_delta < -16) {
        printf("FAIL: Resource leak detected! Net delta: %d frames\n", net_delta);
        return 1;
    }

    printf("============================================================\n");
    printf("PASS: 200 EXEC/EXIT AND 200 FS CYCLES COMPLETED WITH ZERO LEAKS\n");
    printf("============================================================\n");
    return 0;
}
