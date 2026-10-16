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
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
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

    if (cmd == F_SETFL) {
        int karg = 0;
        if (arg & _FAPPEND)   karg |= 0x0400; /* Kernel O_APPEND */
        if (arg & 0x0400)     karg |= 0x0400; /* Direct Kernel O_APPEND */
        if (arg & _FNONBLOCK) karg |= 0x4000; /* Kernel O_NONBLOCK */
        if (arg & 0x0800)     karg |= 0x4000; /* Linux O_NONBLOCK */
        arg = karg;
    }

    int ret = __syscall3(SYS_FCNTL, fd, cmd, arg);
    if (ret >= 0 && cmd == F_GETFL) {
        int uflags = ret & 3; /* O_RDONLY, O_WRONLY, O_RDWR */
        if (ret & 0x0400) uflags |= _FAPPEND;   /* Kernel O_APPEND -> Newlib _FAPPEND */
        if (ret & 0x4000) uflags |= _FNONBLOCK; /* Kernel O_NONBLOCK -> Newlib _FNONBLOCK */
        if (ret & 0x0800) uflags |= _FNONBLOCK; /* Linux O_NONBLOCK */
        return uflags;
    }
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

int msync(void *addr, size_t length, int flags) {
    int ret = __syscall3(SYS_MSYNC, (long)addr, (long)length, (long)flags);
    return __check_syscall_err(ret);
}

int shm_open(const char *name, int oflag, mode_t mode) {
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    while (*name == '/') name++;
    if (*name == '\0') {
        errno = EINVAL;
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/dev/shm/%s", name);
    return open(path, oflag, mode);
}

int shm_unlink(const char *name) {
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    while (*name == '/') name++;
    if (*name == '\0') {
        errno = EINVAL;
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/dev/shm/%s", name);
    return unlink(path);
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

/* Stdio POSIX file locking (single-threaded stubs) */
void flockfile(FILE *file) { (void)file; }
void funlockfile(FILE *file) { (void)file; }
int ftrylockfile(FILE *file) { (void)file; return 0; }

/* popen / pclose */
static struct {
    FILE *fp;
    pid_t pid;
} popen_list[16];

FILE *popen(const char *command, const char *type) {
    if (!command || !type) return NULL;
    int is_read = (type[0] == 'r');
    int is_write = (type[0] == 'w');
    if (!is_read && !is_write) {
        errno = EINVAL;
        return NULL;
    }

    int pfd[2];
    if (pipe(pfd) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }

    if (pid == 0) {
        if (is_read) {
            close(pfd[0]);
            dup2(pfd[1], 1);
            close(pfd[1]);
        } else {
            close(pfd[1]);
            dup2(pfd[0], 0);
            close(pfd[0]);
        }
        char *const argv[] = { "sh", "-c", (char*)command, NULL };
        execvp("sh", argv);
        execvp("sh.elf", argv);
        exit(127);
    }

    FILE *fp = NULL;
    if (is_read) {
        close(pfd[1]);
        fp = fdopen(pfd[0], "r");
    } else {
        close(pfd[0]);
        fp = fdopen(pfd[1], "w");
    }

    if (!fp) {
        if (is_read) close(pfd[0]);
        else close(pfd[1]);
        return NULL;
    }

    for (int i = 0; i < 16; i++) {
        if (!popen_list[i].fp) {
            popen_list[i].fp = fp;
            popen_list[i].pid = pid;
            break;
        }
    }
    return fp;
}

int pclose(FILE *stream) {
    if (!stream) return -1;
    pid_t pid = -1;
    int idx = -1;
    for (int i = 0; i < 16; i++) {
        if (popen_list[i].fp == stream) {
            pid = popen_list[i].pid;
            idx = i;
            break;
        }
    }
    fclose(stream);
    if (idx >= 0) {
        popen_list[idx].fp = NULL;
        popen_list[idx].pid = 0;
    }
    if (pid <= 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return status;
}

/* POSIX getline / getdelim */
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t pos = 0;
    int c = 0;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 2 > *n) {
            size_t new_size = *n * 2;
            char *new_ptr = realloc(*lineptr, new_size);
            if (!new_ptr) return -1;
            *lineptr = new_ptr;
            *n = new_size;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }
    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

ssize_t __getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    return getdelim(lineptr, n, delim, stream);
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

ssize_t __getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

int ftruncate(int fd, off_t length) {
    int ret = __syscall2(SYS_FTRUNCATE, fd, (long)length);
    return __check_syscall_err(ret);
}

int truncate(const char *path, off_t length) {
    int ret = __syscall2(SYS_TRUNCATE, (long)path, (long)length);
    return __check_syscall_err(ret);
}

int access(const char *path, int mode) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return -1;
    }
    (void) mode;
    return 0;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    int ret = __syscall3(SYS_POLL, (long)fds, (long)nfds, (long)timeout);
    return __check_syscall_err(ret);
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd pfds[FD_SETSIZE];
    int pfd_map[FD_SETSIZE];
    nfds_t pcount = 0;

    for (int fd = 0; fd < nfds; fd++) {
        short events = 0;
        if (readfds && FD_ISSET(fd, readfds)) {
            events |= POLLIN;
        }
        if (writefds && FD_ISSET(fd, writefds)) {
            events |= POLLOUT;
        }
        if (exceptfds && FD_ISSET(fd, exceptfds)) {
            events |= POLLPRI;
        }
        if (events != 0) {
            pfds[pcount].fd = fd;
            pfds[pcount].events = events;
            pfds[pcount].revents = 0;
            pfd_map[pcount] = fd;
            pcount++;
        }
    }

    int timeout_ms = -1;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0) {
            errno = EINVAL;
            return -1;
        }
        timeout_ms = timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000;
    }

    int pret = poll(pfds, pcount, timeout_ms);
    if (pret < 0) {
        return -1;
    }

    if (readfds) FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);

    int ready_count = 0;
    for (nfds_t i = 0; i < pcount; i++) {
        int fd = pfd_map[i];
        int set_any = 0;
        if (readfds && (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fd, readfds);
            set_any = 1;
        }
        if (writefds && (pfds[i].revents & (POLLOUT | POLLERR))) {
            FD_SET(fd, writefds);
            set_any = 1;
        }
        if (exceptfds && (pfds[i].revents & POLLPRI)) {
            FD_SET(fd, exceptfds);
            set_any = 1;
        }
        if (set_any) {
            ready_count++;
        }
    }

    return ready_count;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask) {
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000;
        tvp = &tv;
    }

    sigset_t origmask;
    if (sigmask) {
        if (sigprocmask(SIG_SETMASK, sigmask, &origmask) < 0) {
            return -1;
        }
    }

    int ret = select(nfds, readfds, writefds, exceptfds, tvp);

    if (sigmask) {
        int saved_errno = errno;
        sigprocmask(SIG_SETMASK, &origmask, NULL);
        errno = saved_errno;
    }

    return ret;
}

/* Sockets API */
int socket(int domain, int type, int protocol) {
    int ret = __syscall3(SYS_SOCKET, domain, type, protocol);
    return __check_syscall_err(ret);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int ret = __syscall3(SYS_BIND, sockfd, (long)addr, (long)addrlen);
    return __check_syscall_err(ret);
}

int listen(int sockfd, int backlog) {
    int ret = __syscall2(SYS_LISTEN, sockfd, backlog);
    return __check_syscall_err(ret);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int ret = __syscall4(SYS_ACCEPT, sockfd, (long)addr, (long)addrlen, 0);
    return __check_syscall_err(ret);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int ret = __syscall3(SYS_CONNECT, sockfd, (long)addr, (long)addrlen);
    return __check_syscall_err(ret);
}

int shutdown(int sockfd, int how) {
    int ret = __syscall2(SYS_SHUTDOWN, sockfd, how);
    return __check_syscall_err(ret);
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int ret = __syscall3(SYS_GETSOCKNAME, sockfd, (long)addr, (long)addrlen);
    return __check_syscall_err(ret);
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int ret = __syscall3(SYS_GETPEERNAME, sockfd, (long)addr, (long)addrlen);
    return __check_syscall_err(ret);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    int ret = __syscall4(SYS_SOCKETPAIR, domain, type, protocol, (long)sv);
    return __check_syscall_err(ret);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    int ret = __syscall4(SYS_SEND, sockfd, (long)buf, (long)len, flags);
    return (ssize_t) __check_syscall_err(ret);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    int ret = __syscall4(SYS_RECV, sockfd, (long)buf, (long)len, flags);
    return (ssize_t) __check_syscall_err(ret);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    (void) dest_addr;
    (void) addrlen;
    return send(sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    (void) src_addr;
    (void) addrlen;
    return recv(sockfd, buf, len, flags);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    int ret = __syscall5(SYS_GETSOCKOPT, sockfd, level, optname, (long)optval, (long)optlen);
    return __check_syscall_err(ret);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    int ret = __syscall5(SYS_SETSOCKOPT, sockfd, level, optname, (long)optval, (long)optlen);
    return __check_syscall_err(ret);
}

/* PTY Support */
int posix_openpt(int flags) {
    return open("/dev/ptmx", flags);
}

int grantpt(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

int unlockpt(int fd) {
    int unlock = 0;
    return ioctl(fd, TIOCSPTLCK, &unlock);
}

int ptsname_r(int fd, char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    int pty_num = 0;
    if (ioctl(fd, TIOCGPTN, &pty_num) < 0) {
        return -1;
    }
    snprintf(buf, buflen, "/dev/pts/%d", pty_num);
    return 0;
}

char *ptsname(int fd) {
    static char s_ptsname[32];
    if (ptsname_r(fd, s_ptsname, sizeof(s_ptsname)) != 0) {
        return NULL;
    }
    return s_ptsname;
}

int openpty(int *amaster, int *aslave, char *name, const struct termios *termp, const struct winsize *winp) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;

    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        close(master);
        return -1;
    }

    char sname[64];
    if (ptsname_r(master, sname, sizeof(sname)) != 0) {
        close(master);
        return -1;
    }

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }

    if (name) strcpy(name, sname);
    if (termp) tcsetattr(slave, TCSANOW, termp);
    if (winp) ioctl(slave, TIOCSWINSZ, winp);

    *amaster = master;
    *aslave = slave;
    return 0;
}

pid_t forkpty(int *amaster, char *name, const struct termios *termp, const struct winsize *winp) {
    int master, slave;
    if (openpty(&master, &slave, name, termp, winp) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        return 0;
    }

    /* Parent */
    close(slave);
    *amaster = master;
    return pid;
}

int reboot(int cmd) {
    int ret = __syscall4(SYS_REBOOT, 0xfee1dead, 672274793, cmd, 0);
    return __check_syscall_err(ret);
}

