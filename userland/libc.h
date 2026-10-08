/* libc.h - Backwards compatibility shim for MyOS userland utilities */
#ifndef _USERLAND_LIBC_H
#define _USERLAND_LIBC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/myos.h>

#define sys_read(fd, buf, sz)       read(fd, buf, sz)
#define sys_write(fd, buf, sz)      write(fd, buf, sz)
#define sys_open(path, flags, mode) open(path, flags, mode)
#define sys_close(fd)               close(fd)
#define sys_fork()                  fork()
#define sys_exit(...)               exit((0, ##__VA_ARGS__))
#define sys_getpid()                getpid()
#define sys_sbrk(inc)               sbrk(inc)
#define sys_exec(path, argv)        execv(path, (char* const*)(argv))
#define exec(path, argv)            execv(path, (char* const*)(argv))

pid_t getpgrp(void);
int nice(int inc);

#endif /* _USERLAND_LIBC_H */
