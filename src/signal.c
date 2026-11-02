#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "errno.h"
#include "serial.h"
#include "uaccess.h"
#include "timer.h"
#include "fpu.h"
#include <string.h>

typedef enum {
    ACT_TERM,
    ACT_IGN,
    ACT_STOP,
    ACT_CONT
} default_act_t;

static default_act_t get_default_action(int sig) {
    switch (sig) {
        case SIGCHLD:
            return ACT_IGN;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            return ACT_STOP;
        case SIGCONT:
            return ACT_CONT;
        default:
            return ACT_TERM;
    }
}

void signal_init_proc(process_t* p) {
    if (!p) return;
    p->sig_pending = 0;
    p->sig_blocked = 0;
    p->alarm_ticks = 0;
    p->is_stopped = 0;
    p->stopped_reported = 0;
    p->stop_sig = 0;
    p->term_sig = 0;
    for (int i = 0; i < NSIG; i++) {
        p->sig_actions[i].sa_handler = SIG_DFL;
        p->sig_actions[i].sa_mask = 0;
        p->sig_actions[i].sa_flags = 0;
        p->sig_actions[i].sa_restorer = 0;
    }
}

int sig_send(process_t* proc, int sig) {
    if (!proc || proc->state == PROC_UNUSED || proc->state == PROC_ZOMBIE) {
        return -ESRCH;
    }
    if (sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }

    serial_printf("[signal] sending signal %d to PID %d (%s)\n", sig, proc->pid, proc->name);

    if (sig == SIGCONT) {
        proc->sig_pending &= ~(sigmask(SIGSTOP) | sigmask(SIGTSTP) | sigmask(SIGTTIN) | sigmask(SIGTTOU));
        if (proc->is_stopped) {
            proc->is_stopped = 0;
            proc->state = PROC_READY;
            serial_printf("[signal] PID %d resumed by SIGCONT\n", proc->pid);
        }
    } else if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        proc->sig_pending &= ~sigmask(SIGCONT);
    }

    proc->sig_pending |= sigmask(sig);

    /* Wake up waiting process if deliverable */
    if (proc->state == PROC_WAITING) {
        if (!proc->is_stopped || sig == SIGKILL || sig == SIGTERM || sig == SIGINT || sig == SIGQUIT) {
            if ((proc->sig_pending & ~proc->sig_blocked) != 0) {
                if (proc->is_stopped) {
                    proc->is_stopped = 0;
                }
                proc->state = PROC_READY;
                proc->wait_channel = 0;
                proc->num_poll_channels = 0;
                timer_sleep_dequeue(proc);
            }
        }
    }

    return 0;
}

int sig_send_pid(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;

    process_t* me = scheduler_current();
    int count = 0;

    if (pid > 0) {
        process_t* p = process_find_by_pid(pid);
        if (!p) return -ESRCH;
        if (sig == 0) return 0;
        return sig_send(p, sig);
    }

    int target_pgid = (pid == 0) ? (me ? me->pgid : 0) : -pid;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;

        if (pid == -1) {
            if (p->pid > 1) {
                if (sig != 0) sig_send(p, sig);
                count++;
            }
        } else {
            if (p->pgid == target_pgid) {
                if (sig != 0) sig_send(p, sig);
                count++;
            }
        }
    }

    if (count == 0 && pid != -1) return -ESRCH;
    return 0;
}

int signal_has_deliverable(process_t* proc) {
    if (!proc) return 0;
    sigset_t del = proc->sig_pending & ~proc->sig_blocked;
    if (!del) return 0;

    for (int s = 1; s < NSIG; s++) {
        if (del & sigmask(s)) {
            struct sigaction* sa = &proc->sig_actions[s];
            if (sa->sa_handler == SIG_IGN) continue;
            if (sa->sa_handler == SIG_DFL && get_default_action(s) == ACT_IGN) continue;
            return 1;
        }
    }
    return 0;
}

void signal_handle_pending(struct registers* regs) {
    if ((regs->cs & 3) != 3) return;

    process_t* me = scheduler_current();
    if (!me || me->is_kernel_task || me->state == PROC_ZOMBIE || me->state == PROC_UNUSED)
        return;

    sigset_t deliverable = me->sig_pending & ~me->sig_blocked;
    if (!deliverable) return;

    int sig = 0;
    for (int s = 1; s < NSIG; s++) {
        if (deliverable & sigmask(s)) {
            struct sigaction* sa = &me->sig_actions[s];
            if (sa->sa_handler == SIG_IGN) {
                me->sig_pending &= ~sigmask(s);
                continue;
            }
            if (sa->sa_handler == SIG_DFL && get_default_action(s) == ACT_IGN) {
                me->sig_pending &= ~sigmask(s);
                continue;
            }
            sig = s;
            break;
        }
    }
    if (sig == 0) return;

    me->sig_pending &= ~sigmask(sig);
    struct sigaction* sa = &me->sig_actions[sig];

    if (sa->sa_handler == SIG_IGN) {
        serial_printf("[signal] PID %d ignored signal %d\n", me->pid, sig);
        return;
    }

    if (sa->sa_handler == SIG_DFL) {
        default_act_t def = get_default_action(sig);
        if (def == ACT_IGN) return;
        if (def == ACT_CONT) return;
        if (def == ACT_STOP) {
            serial_printf("[signal] PID %d (%s) stopped by signal %d\n", me->pid, me->name, sig);
            me->is_stopped = 1;
            me->stopped_reported = 0;
            me->stop_sig = sig;
            me->state = PROC_WAITING;
            process_t* parent = process_find_by_pid(me->ppid);
            if (parent) {
                sig_send(parent, SIGCHLD);
            }
            wake_waiters_for(me->pid);
            while (me->is_stopped && me->state == PROC_WAITING) {
                scheduler_yield();
            }
            if (me->sig_pending & (sigmask(SIGKILL) | sigmask(SIGTERM) | sigmask(SIGINT) | sigmask(SIGQUIT))) {
                signal_handle_pending(regs);
            }
            return;
        }

        /* ACT_TERM */
        serial_printf("[signal] PID %d (%s) terminated by signal %d (default action)\n",
                      me->pid, me->name, sig);
        me->term_sig = sig;
        process_t* parent = process_find_by_pid(me->ppid);
        if (parent) {
            sig_send(parent, SIGCHLD);
        }
        scheduler_exit_current(128 + sig);
        return;
    }

    /* Deliver to user-space handler */
    serial_printf("[signal] delivering signal %d to PID %d handler %p\n",
                  sig, me->pid, sa->sa_handler);

    uint64_t sp = regs->rsp;
    sp -= sizeof(struct sigframe);
    sp &= ~15ULL; /* 16-byte align */

    struct sigframe frame __attribute__((aligned(16)));
    memset(&frame, 0, sizeof(frame));

    frame.sc.r15 = regs->r15;
    frame.sc.r14 = regs->r14;
    frame.sc.r13 = regs->r13;
    frame.sc.r12 = regs->r12;
    frame.sc.r11 = regs->r11;
    frame.sc.r10 = regs->r10;
    frame.sc.r9  = regs->r9;
    frame.sc.r8  = regs->r8;
    frame.sc.rbp = regs->rbp;
    frame.sc.rsi = regs->rsi;
    frame.sc.rdi = regs->rdi;
    frame.sc.rdx = regs->rdx;
    frame.sc.rcx = regs->rcx;
    frame.sc.rbx = regs->rbx;
    frame.sc.rax = regs->rax;
    frame.sc.int_no = regs->int_no;
    frame.sc.err_code = regs->err_code;
    frame.sc.rip = regs->rip;
    frame.sc.cs = regs->cs;
    frame.sc.rflags = regs->rflags;
    frame.sc.rsp = regs->rsp;
    frame.sc.ss = regs->ss;
    frame.sc.old_mask = me->sig_blocked;
    frame.sc.fpstate = sp + (uint64_t) offsetof(struct sigframe, fpu_state);

    fpu_save(me);
    memcpy(frame.fpu_state, fpu_get_state_ptr(me), 512);

    /* Trampoline: mov $15, %rax (__NR_rt_sigreturn); syscall; int $0x80 */
    frame.trampoline[0] = 0x48;
    frame.trampoline[1] = 0xC7;
    frame.trampoline[2] = 0xC0;
    frame.trampoline[3] = 15; /* __NR_rt_sigreturn */
    frame.trampoline[4] = 0x00;
    frame.trampoline[5] = 0x00;
    frame.trampoline[6] = 0x00;
    frame.trampoline[7] = 0x0F; /* syscall */
    frame.trampoline[8] = 0x05;
    frame.trampoline[9] = 0xCD; /* int $0x80 */
    frame.trampoline[10] = 0x80;
    frame.trampoline[11] = 0x0F;
    frame.trampoline[12] = 0x0B;

    if ((sa->sa_flags & SA_RESTORER) && sa->sa_restorer) {
        frame.ret_addr = (uint64_t) sa->sa_restorer;
    } else {
        frame.ret_addr = sp + (uint64_t) offsetof(struct sigframe, trampoline);
    }

    if (copy_to_user((void*)sp, &frame, sizeof(struct sigframe)) != 0) {
        serial_printf("[signal] PID %d stack fault building signal frame\n", me->pid);
        scheduler_exit_current(139);
        return;
    }

    regs->rsp = sp;
    regs->rip = (uint64_t) sa->sa_handler;
    regs->rdi = sig; /* Arg1 in AMD64 ABI */

    me->sig_blocked |= sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER)) {
        me->sig_blocked |= sigmask(sig);
    }
}

int sys_kill(int pid, int sig) {
    return sig_send_pid(pid, sig);
}

int sys_sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }

    process_t* me = scheduler_current();
    if (!me) return -ESRCH;

    if (oldact) {
        if (copy_to_user(oldact, &me->sig_actions[sig], sizeof(struct sigaction)) != 0)
            return -EFAULT;
    }

    if (act) {
        struct sigaction new_act;
        if (copy_from_user(&new_act, act, sizeof(struct sigaction)) != 0)
            return -EFAULT;
        me->sig_actions[sig] = new_act;
    }

    return 0;
}

int sys_sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    process_t* me = scheduler_current();
    if (!me) return -ESRCH;

    if (oldset) {
        if (copy_to_user(oldset, &me->sig_blocked, sizeof(sigset_t)) != 0)
            return -EFAULT;
    }

    if (set) {
        sigset_t new_set;
        if (copy_from_user(&new_set, set, sizeof(sigset_t)) != 0)
            return -EFAULT;

        if (how == SIG_BLOCK) {
            me->sig_blocked |= new_set;
        } else if (how == SIG_UNBLOCK) {
            me->sig_blocked &= ~new_set;
        } else if (how == SIG_SETMASK) {
            me->sig_blocked = new_set;
        } else {
            return -EINVAL;
        }
        me->sig_blocked &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));
    }

    return 0;
}

int sys_sigreturn(struct registers* regs) {
    process_t* me = scheduler_current();
    if (!me) return -ESRCH;

    struct sigcontext* u_sc = (struct sigcontext*) regs->rsp;
    struct sigcontext sc;
    if (copy_from_user(&sc, u_sc, sizeof(struct sigcontext)) != 0) {
        return -EFAULT;
    }

    regs->r15 = sc.r15;
    regs->r14 = sc.r14;
    regs->r13 = sc.r13;
    regs->r12 = sc.r12;
    regs->r11 = sc.r11;
    regs->r10 = sc.r10;
    regs->r9  = sc.r9;
    regs->r8  = sc.r8;
    regs->rbp = sc.rbp;
    regs->rsi = sc.rsi;
    regs->rdi = sc.rdi;
    regs->rdx = sc.rdx;
    regs->rcx = sc.rcx;
    regs->rbx = sc.rbx;
    regs->rax = sc.rax;
    regs->rip = sc.rip;
    regs->rsp = sc.rsp;
    regs->rflags = sc.rflags;
    regs->cs = sc.cs;
    regs->ss = sc.ss;

    me->sig_blocked = sc.old_mask & ~(sigmask(SIGKILL) | sigmask(SIGSTOP));

    if (sc.fpstate) {
        uint8_t* p_fpu = fpu_get_state_ptr(me);
        if (p_fpu && copy_from_user(p_fpu, (const void*)sc.fpstate, 512) == 0) {
            fpu_restore(me);
        }
    }

    serial_printf("[signal] PID %d sigreturn restored rip=0x%016lx rsp=0x%016lx\n",
                  me->pid, regs->rip, regs->rsp);
    return (int) regs->rax;
}

int sys_pause(void) {
    process_t* me = scheduler_current();
    if (!me) return -ESRCH;
    while (!signal_has_deliverable(me)) {
        scheduler_wait_channel(&me->sig_pending);
    }
    return -EINTR;
}

unsigned int sys_alarm(unsigned int seconds) {
    process_t* me = scheduler_current();
    if (!me) return 0;

    uint32_t old_remaining = (me->alarm_ticks + 99) / 100;
    me->alarm_ticks = seconds * 100;
    return old_remaining;
}
