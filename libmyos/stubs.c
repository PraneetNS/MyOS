#include "syscall.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

struct kernel_stat {
    unsigned int k_dev;
    unsigned int k_ino;
    unsigned int k_mode;
    unsigned int k_nlink;
    unsigned int k_size;
    unsigned int k_blksize;
    unsigned int k_blocks;
    unsigned int k_atime;
    unsigned int k_mtime;
    unsigned int k_ctime;
};

static void translate_kernel_stat(const struct kernel_stat* kst, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = (dev_t) kst->k_dev;
    st->st_ino = (ino_t) kst->k_ino;

    mode_t m = 0;
    unsigned int km = kst->k_mode;
    if ((km & 0xF000) == 0x8000) m |= S_IFREG;
    else if ((km & 0xF000) == 0x4000) m |= S_IFDIR;
    else if ((km & 0xF000) == 0x2000) m |= S_IFCHR;

    m |= (km & 0777);
    if ((m & S_IFMT) == 0) m |= S_IFREG;
    if ((m & 0777) == 0) {
        if (S_ISDIR(m)) m |= 0755;
        else m |= 0644;
    }
    st->st_mode = m;

    st->st_nlink = (nlink_t) (kst->k_nlink ? kst->k_nlink : 1);
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = (off_t) kst->k_size;
    st->st_atim.tv_sec = kst->k_atime;
    st->st_atim.tv_nsec = 0;
    st->st_mtim.tv_sec = kst->k_mtime;
    st->st_mtim.tv_nsec = 0;
    st->st_ctim.tv_sec = kst->k_ctime;
    st->st_ctim.tv_nsec = 0;
    st->st_blksize = (blksize_t) (kst->k_blksize ? kst->k_blksize : 512);
    st->st_blocks = (blkcnt_t) (kst->k_blocks ? kst->k_blocks : ((kst->k_size + 511) / 512));
}

void _exit(int status) {
    __syscall1(SYS_EXIT, status);
    while (1) {
        __asm__ __volatile__ ("hlt");
    }
}

static int translate_open_flags(int flags) {
    int kflags = 0;
    int acc = flags & 3; /* O_RDONLY, O_WRONLY, O_RDWR */
    kflags |= acc;

    /* Newlib -> Kernel flags */
    if (flags & 0x0200) /* _FCREAT */
        kflags |= 0x0040; /* Kernel O_CREAT */
    if (flags & 0x0400) /* _FTRUNC */
        kflags |= 0x0200; /* Kernel O_TRUNC */
    if (flags & 0x0008) /* _FAPPEND */
        kflags |= 0x0400; /* Kernel O_APPEND */
    if (flags & 0x40000) /* _FNOINHERIT / O_CLOEXEC */
        kflags |= 0x80000; /* Kernel O_CLOEXEC */

    /* Also preserve direct kernel flags if passed: */
    if (flags & 0x0040)
        kflags |= 0x0040;
    if (flags & 0x80000)
        kflags |= 0x80000;

    return kflags;
}

int _open(const char *file, int flags, int mode) {
    int kflags = translate_open_flags(flags);
    int ret = __syscall3(SYS_OPEN, (long)file, kflags, mode);
    return __check_syscall_err(ret);
}

int _close(int fd) {
    int ret = __syscall1(SYS_CLOSE, fd);
    return __check_syscall_err(ret);
}

_READ_WRITE_RETURN_TYPE _read(int fd, void *buf, size_t count) {
    int ret = __syscall3(SYS_READ, fd, (long)buf, count);
    return __check_syscall_err(ret);
}

_READ_WRITE_RETURN_TYPE _write(int fd, const void *buf, size_t count) {
    int ret = __syscall3(SYS_WRITE, fd, (long)buf, count);
    return __check_syscall_err(ret);
}

off_t _lseek(int fd, off_t offset, int whence) {
    int ret = __syscall3(SYS_LSEEK, fd, offset, whence);
    return __check_syscall_err(ret);
}

int _fstat(int fd, struct stat *st) {
    struct kernel_stat kst;
    int ret = __syscall2(SYS_FSTAT, fd, (long)&kst);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    translate_kernel_stat(&kst, st);
    return 0;
}

int _stat(const char *file, struct stat *st) {
    struct kernel_stat kst;
    int ret = __syscall2(SYS_STAT, (long)file, (long)&kst);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    translate_kernel_stat(&kst, st);
    return 0;
}

int _isatty(int fd) {
    /* Use SYS_TCGETATTR (55) to check whether fd is a terminal */
    uint8_t termios_buf[128];
    int ret = __syscall2(SYS_TCGETATTR, fd, (long)termios_buf);
    if (ret == 0) return 1;
    errno = ENOTTY;
    return 0;
}

void *_sbrk(ptrdiff_t incr) {
    int ret = __syscall1(SYS_SBRK, (long)incr);
    if (ret == -1) {
        errno = ENOMEM;
        return (void*)-1;
    }
    return (void*) ret;
}

int _kill(pid_t pid, int sig) {
    int ret = __syscall2(SYS_KILL, pid, sig);
    return __check_syscall_err(ret);
}

pid_t _getpid(void) {
    return (pid_t) __syscall0(SYS_GETPID);
}

int _link(const char *oldname, const char *newname) {
    (void) oldname;
    (void) newname;
    errno = ENOSYS;
    return -1;
}

int _unlink(const char *file) {
    int ret = __syscall1(SYS_UNLINK, (long)file);
    return __check_syscall_err(ret);
}

pid_t _fork(void) {
    int ret = __syscall0(SYS_FORK);
    return __check_syscall_err(ret);
}

int _execve(const char *name, char *const argv[], char *const envp[]) {
    int ret = __syscall3(SYS_EXEC, (long)name, (long)argv, (long)envp);
    return __check_syscall_err(ret);
}

pid_t _wait(int *status) {
    int ret = __syscall3(SYS_WAITPID, -1, (long)status, 0);
    return __check_syscall_err(ret);
}

clock_t _times(struct tms *buf) {
    int ticks = __syscall0(SYS_TICKS);
    if (buf) {
        buf->tms_utime = ticks;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return (clock_t) ticks;
}

int _gettimeofday(struct timeval *tv, void *tz) {
    int ret = __syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)tz);
    return __check_syscall_err(ret);
}

/* POSIX aliases for standard calls */
int open(const char *file, int flags, ...) {
    mode_t mode = 0;
    return _open(file, flags, mode);
}

int close(int fd) { return _close(fd); }
_READ_WRITE_RETURN_TYPE read(int fd, void *buf, size_t count) { return _read(fd, buf, count); }
_READ_WRITE_RETURN_TYPE write(int fd, const void *buf, size_t count) { return _write(fd, buf, count); }
off_t lseek(int fd, off_t offset, int whence) { return _lseek(fd, offset, whence); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int stat(const char *file, struct stat *st) { return _stat(file, st); }
int isatty(int fd) { return _isatty(fd); }
void *sbrk(ptrdiff_t incr) { return _sbrk(incr); }
int kill(pid_t pid, int sig) { return _kill(pid, sig); }
pid_t getpid(void) { return _getpid(); }
int link(const char *oldname, const char *newname) { return _link(oldname, newname); }
int unlink(const char *file) { return _unlink(file); }
pid_t fork(void) { return _fork(); }
int execve(const char *name, char *const argv[], char *const envp[]) { return _execve(name, argv, envp); }
pid_t wait(int *status) { return _wait(status); }
clock_t times(struct tms *buf) { return _times(buf); }
int gettimeofday(struct timeval *tv, void *tz) { return _gettimeofday(tv, tz); }
