#include "syscall.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <termios.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

extern char **environ;
extern void __sigreturn_restorer(void);

/* Kernel dirent structure for SYS_GETDENTS */
struct kernel_dirent {
    unsigned int d_ino;
    char d_name[64];
    unsigned int d_type; /* 1 = file, 2 = dir */
    unsigned int d_size;
};

/* Process & Execution */
pid_t waitpid(pid_t pid, int *status, int options) {
    int ret = __syscall3(SYS_WAITPID, pid, (long)status, options);
    return __check_syscall_err(ret);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int exec(const char *path, char *const argv[]) {
    return execv(path, argv);
}

int execvp(const char *file, char *const argv[]) {
    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/')) {
        return execv(file, argv);
    }

    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/bin:/";
    }

    char full_path[256];
    const char *p = path;
    while (*p) {
        const char *next = strchr(p, ':');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len + 1 + strlen(file) + 1 < sizeof(full_path)) {
            memcpy(full_path, p, len);
            if (len > 0 && full_path[len - 1] != '/') {
                full_path[len] = '/';
                len++;
            }
            strcpy(full_path + len, file);
            execve(full_path, argv, environ);
        }
        if (!next) break;
        p = next + 1;
    }

    errno = ENOENT;
    return -1;
}

int execl(const char *path, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);

    int count = 1;
    while (va_arg(ap, const char*)) {
        count++;
    }
    va_end(ap);

    char **argv = (char**) malloc((count + 1) * sizeof(char*));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    va_start(ap, arg0);
    argv[0] = (char*) arg0;
    for (int i = 1; i < count; i++) {
        argv[i] = (char*) va_arg(ap, const char*);
    }
    argv[count] = NULL;
    va_end(ap);

    int ret = execv(path, argv);
    free(argv);
    return ret;
}

/* File & Directory Operations */
int pipe(int pipefd[2]) {
    int ret = __syscall1(SYS_PIPE, (long)pipefd);
    return __check_syscall_err(ret);
}

int dup(int oldfd) {
    int ret = __syscall1(SYS_DUP, oldfd);
    return __check_syscall_err(ret);
}

int dup2(int oldfd, int newfd) {
    int ret = __syscall2(SYS_DUP2, oldfd, newfd);
    return __check_syscall_err(ret);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    int ret = __syscall3(SYS_FCNTL, fd, cmd, arg);
    return __check_syscall_err(ret);
}

int mkdir(const char *path, mode_t mode) {
    int ret = __syscall2(SYS_MKDIR, (long)path, (int)mode);
    return __check_syscall_err(ret);
}

int rmdir(const char *path) {
    int ret = __syscall1(SYS_RMDIR, (long)path);
    return __check_syscall_err(ret);
}

int chdir(const char *path) {
    int ret = __syscall1(SYS_CHDIR, (long)path);
    return __check_syscall_err(ret);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    int ret = __syscall2(SYS_GETCWD, (long)buf, (long)size);
    if ((unsigned long)ret > 0xFFFFF000u) {
        errno = -ret;
        return NULL;
    }
    return buf;
}

int rename(const char *oldpath, const char *newpath) {
    int ret = __syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
    return __check_syscall_err(ret);
}

/* Directory Streams */
DIR *opendir(const char *name) {
    int fd = open(name, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return NULL;
    }

    DIR *dirp = (DIR*) malloc(sizeof(DIR));
    if (!dirp) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    dirp->fd = fd;
    memset(&dirp->ent, 0, sizeof(dirp->ent));
    return dirp;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp || dirp->fd < 0) {
        errno = EBADF;
        return NULL;
    }

    int ret = __syscall3(SYS_GETDENTS, dirp->fd, (long)&dirp->ent, sizeof(dirp->ent));
    if (ret < 0) {
        errno = -ret;
        return NULL;
    }
    if (ret == 0) {
        return NULL; /* EOF */
    }

    return &dirp->ent;
}

int closedir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    int ret = close(dirp->fd);
    free(dirp);
    return ret;
}

void rewinddir(DIR *dirp) {
    if (dirp) {
        lseek(dirp->fd, 0, SEEK_SET);
    }
}

int getdents(int fd, void *dirp, size_t count) {
    int ret = __syscall3(SYS_GETDENTS, fd, (long)dirp, count);
    return __check_syscall_err(ret);
}

/* Signals */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    int ret = __syscall3(SYS_SIGACTION, signum, (long)act, (long)oldact);
    return __check_syscall_err(ret);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    int ret = __syscall3(SYS_SIGPROCMASK, how, (long)set, (long)oldset);
    return __check_syscall_err(ret);
}

typedef void (*sighandler_t)(int);

sighandler_t signal(int signum, sighandler_t handler) {
    struct sigaction act, oldact;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = 0x04000000; /* SA_RESTORER */
    act.sa_restorer = __sigreturn_restorer;

    if (sigaction(signum, &act, &oldact) < 0) {
        return SIG_ERR;
    }
    return oldact.sa_handler;
}

int raise(int sig) {
    return kill(getpid(), sig);
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int) __syscall1(SYS_ALARM, seconds);
}

int pause(void) {
    int ret = __syscall0(SYS_PAUSE);
    return __check_syscall_err(ret);
}

#undef sigemptyset
#undef sigfillset
#undef sigaddset
#undef sigdelset
#undef sigismember

int sigemptyset(sigset_t *set) {
    if (!set) { errno = EINVAL; return -1; }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) { errno = EINVAL; return -1; }
    *set = 0xFFFFFFFFU;
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum > 32) {
        errno = EINVAL;
        return -1;
    }
    *set |= (1U << (signum - 1));
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum > 32) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1U << (signum - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (!set || signum < 1 || signum > 32) {
        errno = EINVAL;
        return -1;
    }
    return (*set & (1U << (signum - 1))) ? 1 : 0;
}

/* Process credentials & groups */
pid_t getppid(void) {
    return (pid_t) __syscall0(SYS_GETPPID);
}

int setpgid(pid_t pid, pid_t pgid) {
    int ret = __syscall2(SYS_SETPGID, pid, pgid);
    return __check_syscall_err(ret);
}

pid_t getpgid(pid_t pid) {
    int ret = __syscall1(SYS_GETPGID, pid);
    return (pid_t) __check_syscall_err(ret);
}

pid_t setsid(void) {
    int ret = __syscall0(SYS_SETSID);
    return (pid_t) __check_syscall_err(ret);
}

pid_t getsid(pid_t pid) {
    int ret = __syscall1(SYS_GETSID, pid);
    return (pid_t) __check_syscall_err(ret);
}

int tcsetpgrp(int fd, pid_t pgrp) {
    return ioctl(fd, TIOCSPGRP, &pgrp);
}

pid_t tcgetpgrp(int fd) {
    pid_t pgrp = 0;
    if (ioctl(fd, TIOCGPGRP, &pgrp) < 0) {
        return -1;
    }
    return pgrp;
}

pid_t getpgrp(void) {
    return getpgid(0);
}

int nice(int inc) {
    int ret = __syscall1(SYS_NICE, inc);
    return __check_syscall_err(ret);
}

uid_t getuid(void) { return 0; }
gid_t getgid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getegid(void) { return 0; }

/* Termios & IOCTL */
int tcgetattr(int fd, struct termios *termios_p) {
    int ret = __syscall2(SYS_TCGETATTR, fd, (long)termios_p);
    return __check_syscall_err(ret);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    int ret = __syscall3(SYS_TCSETATTR, fd, optional_actions, (long)termios_p);
    return __check_syscall_err(ret);
}

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
int cfsetispeed(struct termios *t, speed_t speed) { t->c_ispeed = speed; return 0; }
int cfsetospeed(struct termios *t, speed_t speed) { t->c_ospeed = speed; return 0; }

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void*);
    va_end(ap);

    int ret = __syscall3(SYS_IOCTL, fd, request, (long)arg);
    return __check_syscall_err(ret);
}

/* Memory management */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    int ret = __syscall6(SYS_MMAP, (long)addr, length, prot, flags, fd, offset);
    if ((unsigned long)ret > 0xFFFFF000u) {
        errno = -ret;
        return MAP_FAILED;
    }
    return (void*) ret;
}

int munmap(void *addr, size_t length) {
    int ret = __syscall2(SYS_MUNMAP, (long)addr, length);
    return __check_syscall_err(ret);
}

int mprotect(void *addr, size_t length, int prot) {
    int ret = __syscall3(SYS_MPROTECT, (long)addr, length, prot);
    return __check_syscall_err(ret);
}

/* Sleeping & Clock */
struct kernel_timespec {
    uint32_t tv_sec;
    uint32_t tv_nsec;
};

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    struct kernel_timespec kreq;
    kreq.tv_sec = (uint32_t)req->tv_sec;
    kreq.tv_nsec = (uint32_t)req->tv_nsec;
    struct kernel_timespec krem = { 0, 0 };
    int ret = __syscall2(SYS_NANOSLEEP, (long)&kreq, rem ? (long)&krem : 0);
    if (rem && ret < 0) {
        rem->tv_sec = krem.tv_sec;
        rem->tv_nsec = krem.tv_nsec;
    }
    if (ret < 0) {
        errno = -ret;
        return ret;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req = { seconds, 0 };
    struct timespec rem = { 0, 0 };
    if (nanosleep(&req, &rem) < 0) {
        return (unsigned int) rem.tv_sec;
    }
    return 0;
}

int usleep(useconds_t usec) {
    struct timespec req = { usec / 1000000, (usec % 1000000) * 1000 };
    return nanosleep(&req, NULL);
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    struct kernel_timespec ktp = { 0, 0 };
    int ret = __syscall2(SYS_CLOCK_GETTIME, clk_id, (long)&ktp);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    tp->tv_sec = ktp.tv_sec;
    tp->tv_nsec = ktp.tv_nsec;
    return 0;
}

/* Host & System queries */
int uname(struct utsname *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    strncpy(buf->sysname, "MyOS", sizeof(buf->sysname) - 1);
    strncpy(buf->nodename, "myos", sizeof(buf->nodename) - 1);
    strncpy(buf->release, "1.0.0", sizeof(buf->release) - 1);
    strncpy(buf->version, "Stage 17", sizeof(buf->version) - 1);
    strncpy(buf->machine, "i686", sizeof(buf->machine) - 1);
    buf->sysname[sizeof(buf->sysname) - 1] = '\0';
    buf->nodename[sizeof(buf->nodename) - 1] = '\0';
    buf->release[sizeof(buf->release) - 1] = '\0';
    buf->version[sizeof(buf->version) - 1] = '\0';
    buf->machine[sizeof(buf->machine) - 1] = '\0';
    return 0;
}

int gethostname(char *name, size_t len) {
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    strncpy(name, "myos", len);
    name[len - 1] = '\0';
    return 0;
}

long sysconf(int name) {
    switch (name) {
        case 1: /* _SC_ARG_MAX */
            return 4096;
        case 2: /* _SC_CHILD_MAX */
            return 32;
        case 3: /* _SC_CLK_TCK */
            return 100;
        case 4: /* _SC_NGROUPS_MAX */
            return 1;
        case 5: /* _SC_OPEN_MAX */
            return 32;
        case 8: /* _SC_PAGESIZE */
        case 29: /* _SC_PAGE_SIZE */
            return 4096;
        default:
            return -1;
    }
}

unsigned int sys_free_frames(void) {
    return (unsigned int) __syscall0(SYS_FREE_FRAMES);
}

unsigned int sys_ticks(void) {
    return (unsigned int) __syscall0(SYS_TICKS);
}

int sys_sync(void) {
    return __check_syscall_err(__syscall0(SYS_SYNC));
}

void sync(void) {
    sys_sync();
}

void print_uint(unsigned int n) {
    char buf[12];
    int idx = 0;
    if (n == 0) {
        write(1, "0", 1);
        return;
    }
    char tmp[12];
    while (n > 0) {
        tmp[idx++] = '0' + (n % 10);
        n /= 10;
    }
    for (int i = 0; i < idx; i++) {
        buf[i] = tmp[idx - 1 - i];
    }
    write(1, buf, idx);
}

void print_hex(unsigned int v) {
    char hex[11] = "0x00000000";
    const char *digits = "0123456789ABCDEF";
    for (int i = 9; i >= 2; i--) {
        hex[i] = digits[v & 0xF];
        v >>= 4;
    }
    write(1, hex, 10);
}
