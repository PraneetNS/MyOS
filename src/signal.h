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
#define SIGWINCH   28
#define NSIG       32

typedef uint64_t sigset_t;

#define sigmask(sig) (1ULL << ((sig) - 1))

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
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

/* Context saved on user stack during 64-bit signal delivery */
struct sigcontext {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rsi, rdi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
    sigset_t old_mask;
    uint64_t fpstate;          /* user-space pointer to fpu_state, or 0 */
};

/* Signal frame laid out on user stack (16-byte aligned) */
struct sigframe {
    uint64_t ret_addr;         /* address of restorer or trampoline */
    struct sigcontext sc;      /* saved registers */
    uint8_t fpu_state[512];    /* saved FPU/SSE state */
    uint8_t trampoline[16];    /* mov $119, %rax; syscall; int $0x80 */
} __attribute__((aligned(16)));

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
