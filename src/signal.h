#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

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

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001
#define SA_RESTORER  0x04000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

/* Context saved on user stack during signal delivery */
struct sigcontext {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
    sigset_t old_mask;
    uint32_t fpstate;          /* user-space pointer to fpu_state, or 0 */
};

/* Signal frame laid out on user stack */
struct sigframe {
    uint32_t ret_addr;         /* address of restorer or trampoline */
    int sig;                   /* parameter passed to handler */
    struct sigcontext sc;      /* saved registers */
    uint32_t pad;              /* alignment pad to 16-byte boundary */
    uint8_t fpu_state[512];    /* saved FPU/SSE state */
    uint8_t trampoline[8];     /* mov $119, %eax; int $0x80 */
    uint8_t pad2[8];           /* pad struct to multiple of 16 */
};

struct process;

void signal_init_proc(struct process* p);
int sig_send(struct process* proc, int sig);
int sig_send_pid(int pid, int sig);
int signal_has_deliverable(struct process* proc);
void signal_handle_pending(struct registers* regs);

int sys_kill(int pid, int sig);
int sys_sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);
int sys_sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int sys_sigreturn(struct registers* regs);
int sys_pause(void);
unsigned int sys_alarm(unsigned int seconds);

#endif
