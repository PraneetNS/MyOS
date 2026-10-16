#ifndef _PTY_H
#define _PTY_H

#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t buflen);

int openpty(int *amaster, int *aslave, char *name, const struct termios *termp, const struct winsize *winp);
pid_t forkpty(int *amaster, char *name, const struct termios *termp, const struct winsize *winp);

#ifdef __cplusplus
}
#endif

#endif /* _PTY_H */
