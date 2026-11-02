#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/random.h>
#include <signal.h>
#include <errno.h>

static volatile int got_sigusr1 = 0;

static void sigusr1_handler(int sig) {
    (void)sig;
    got_sigusr1 = 1;
}

int main(int argc, char** argv) {
    printf("[musltest] starting (argc=%d, argv0=%s, pid=%d)\n", argc, argv[0], getpid());

    /* 1. malloc / realloc / free */
    char* p = malloc(128 * 1024);
    if (!p) { printf("FAIL: malloc\n"); return 1; }
    memset(p, 0x5A, 128 * 1024);
    p = realloc(p, 256 * 1024);
    if (!p || (unsigned char)p[1000] != 0x5A) { printf("FAIL: realloc\n"); return 1; }
    free(p);
    printf("PASS: musl malloc/realloc/free\n");

    /* 2. stdio + openat/fstat/unlinkat on /tmp */
    FILE* f = fopen("/tmp/musl_io.txt", "w+");
    if (!f) { printf("FAIL: fopen (%d)\n", errno); return 1; }
    fprintf(f, "hello from musl %d\n", 2026);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    char line[128] = {0};
    if (!fgets(line, sizeof(line), f) || strcmp(line, "hello from musl 2026\n") != 0) {
        printf("FAIL: fgets (%s)\n", line);
        return 1;
    }
    fclose(f);

    struct stat st;
    if (stat("/tmp/musl_io.txt", &st) != 0 || st.st_size != 21) {
        printf("FAIL: stat size=%ld\n", (long)st.st_size);
        return 1;
    }

    /* 3. symlink / readlink / lstat */
    unlink("/tmp/musl_lnk");
    if (symlink("/tmp/musl_io.txt", "/tmp/musl_lnk") != 0) {
        printf("FAIL: symlink (%d)\n", errno);
        return 1;
    }
    char target[128] = {0};
    ssize_t rlen = readlink("/tmp/musl_lnk", target, sizeof(target) - 1);
    if (rlen <= 0 || strcmp(target, "/tmp/musl_io.txt") != 0) {
        printf("FAIL: readlink (%zd, %s)\n", rlen, target);
        return 1;
    }
    struct stat lst;
    if (lstat("/tmp/musl_lnk", &lst) != 0 || !S_ISLNK(lst.st_mode)) {
        printf("FAIL: lstat mode=0%o\n", lst.st_mode);
        return 1;
    }
    FILE* fl = fopen("/tmp/musl_lnk", "r");
    if (!fl) { printf("FAIL: fopen symlink\n"); return 1; }
    memset(line, 0, sizeof(line));
    fgets(line, sizeof(line), fl);
    fclose(fl);
    if (strcmp(line, "hello from musl 2026\n") != 0) {
        printf("FAIL: read via symlink\n");
        return 1;
    }
    unlink("/tmp/musl_lnk");
    unlink("/tmp/musl_io.txt");
    printf("PASS: musl stdio, stat, symlink, readlink, lstat\n");

    /* 4. opendir / readdir */
    DIR* d = opendir("/bin");
    if (!d) { printf("FAIL: opendir /bin\n"); return 1; }
    int entries = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        entries++;
    }
    closedir(d);
    if (entries < 5) { printf("FAIL: readdir count=%d\n", entries); return 1; }
    printf("PASS: musl opendir/readdir (/bin entries=%d)\n", entries);

    /* 5. pipe + fork + waitpid */
    int pfd[2];
    if (pipe(pfd) != 0) { printf("FAIL: pipe\n"); return 1; }
    pid_t pid = fork();
    if (pid < 0) { printf("FAIL: fork\n"); return 1; }
    if (pid == 0) {
        close(pfd[0]);
        const char* msg = "child_ok";
        write(pfd[1], msg, 8);
        close(pfd[1]);
        _exit(42);
    }
    close(pfd[1]);
    char cbuf[16] = {0};
    read(pfd[0], cbuf, 8);
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (strcmp(cbuf, "child_ok") != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        printf("FAIL: fork/pipe/waitpid (cbuf=%s status=0x%x)\n", cbuf, status);
        return 1;
    }
    printf("PASS: musl fork/pipe/waitpid\n");

    /* 6. signals (sigaction + kill + SA_RESTORER) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        printf("FAIL: sigaction\n");
        return 1;
    }
    kill(getpid(), SIGUSR1);
    if (!got_sigusr1) {
        printf("FAIL: signal not delivered\n");
        return 1;
    }
    printf("PASS: musl sigaction + kill + rt_sigreturn\n");

    /* 7. uname + sysinfo + getrandom + /proc/self/exe + /proc/self/maps */
    struct utsname uts;
    if (uname(&uts) != 0 || strcmp(uts.machine, "x86_64") != 0) {
        printf("FAIL: uname\n");
        return 1;
    }
    unsigned char rnd[16] = {0};
    if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd)) {
        printf("FAIL: getrandom\n");
        return 1;
    }
    char exepath[128] = {0};
    if (readlink("/proc/self/exe", exepath, sizeof(exepath) - 1) <= 0) {
        printf("FAIL: /proc/self/exe\n");
        return 1;
    }
    FILE* fmaps = fopen("/proc/self/maps", "r");
    if (!fmaps) { printf("FAIL: /proc/self/maps\n"); return 1; }
    char mapline[128] = {0};
    fgets(mapline, sizeof(mapline), fmaps);
    fclose(fmaps);
    if (strlen(mapline) < 10) { printf("FAIL: maps empty\n"); return 1; }
    printf("PASS: musl uname(%s), getrandom, /proc/self/exe(%s), /proc/self/maps\n", uts.release, exepath);

    printf("ALL MUSL TESTS PASSED\n");
    return 0;
}

