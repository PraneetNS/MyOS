#ifndef _SYS_SIGNAL_H
#define _SYS_SIGNAL_H

#include <sys/types.h>
#include <stdint.h>

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGSTKFLT  16
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define SIGTTIN    21
#define SIGTTOU    22
#define NSIG       32

typedef uint32_t sigset_t;

#define sigmask(sig) (1U << ((sig) - 1))

#define SIG_DFL ((void (*)(int)) 0)
#define SIG_IGN ((void (*)(int)) 1)
#define SIG_ERR ((void (*)(int)) -1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001
#define SA_RESTORER  0x04000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000

typedef void (*sighandler_t)(int);
typedef void (*_sig_func_ptr)(int);

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
sighandler_t signal(int signum, sighandler_t handler);
int kill(pid_t pid, int sig);
int raise(int sig);
int pause(void);

#define sigemptyset(what)     (*(what) = 0, 0)
#define sigfillset(what)      (*(what) = 0xFFFFFFFFU, 0)
#define sigaddset(what,sig)   (*(what) |= (1U<<((sig)-1)), 0)
#define sigdelset(what,sig)   (*(what) &= ~(1U<<((sig)-1)), 0)
#define sigismember(what,sig) (((*(what)) & (1U<<((sig)-1))) != 0)

int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

#endif
