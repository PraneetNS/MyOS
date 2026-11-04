#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vmm.h"
#include "fs.h"
#include "idt.h" /* struct registers -- forked children resume from a saved snapshot of it */

#define MAX_PROCESSES 128
#define PROC_KERNEL_STACK_SIZE 65536
#define MAX_FDS 32               /* 32 fds per process; 0=stdin, 1=stdout, 2=stderr reserved */

/* Canonical 64-bit Address Layout:
   ELF text starts at 0x0000000000400000 (4MB)
   Heap Base: 0x0000000010000000 (256MB)
   Heap Max:  0x0000000080000000 (2GB max heap)
   User Stack Top: 0x00007FFFFFFFE000 (below top of lower canonical half)
   User Stack Max Growth: 16MB
*/
#define HEAP_BASE 0x0000000010000000ULL
#define HEAP_MAX  (HEAP_BASE + 2ULL * 1024ULL * 1024ULL * 1024ULL)

#define USER_STACK_TOP        0x00007FFFFFFFE000ULL
#define USER_STACK_MAX_GROWTH (16ULL * 1024ULL * 1024ULL) /* 16MB max stack */
#define USER_STACK_BOTTOM     (USER_STACK_TOP - USER_STACK_MAX_GROWTH)
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101

static inline void wrmsr64(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = (val >> 32) & 0xFFFFFFFF;
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr64(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}
#define USER_STACK_GUARD_GAP  (4096ULL)

#include "vfs.h"
#include "signal.h"

/* Linux Clone Flags */
#define CSIGNAL              0x000000ff /* Signal mask on exit */
#define CLONE_VM             0x00000100 /* Set if VM shared between processes */
#define CLONE_FS             0x00000200 /* Set if fs info shared between processes */
#define CLONE_FILES          0x00000400 /* Set if open files shared between processes */
#define CLONE_SIGHAND        0x00000800 /* Set if signal handlers and blocked signals shared */
#define CLONE_PIDFD          0x00001000 /* Set if a pidfd should be placed in parent */
#define CLONE_PTRACE         0x00002000 /* Set if tracing continues on the child */
#define CLONE_VFORK          0x00004000 /* Set if the parent wants the child to wake it on start_exec */
#define CLONE_PARENT         0x00008000 /* Set if we want to have the same parent as the cloner */
#define CLONE_THREAD         0x00010000 /* Same thread group? */
#define CLONE_NEWNS          0x00020000 /* New mount namespace group */
#define CLONE_SYSVSEM        0x00040000 /* Share system V SEM_UNDO semantics */
#define CLONE_SETTLS         0x00080000 /* Create a new TLS for the child */
#define CLONE_PARENT_SETTID  0x00100000 /* Store the child TID in the parent's memory */
#define CLONE_CHILD_CLEARTID 0x00200000 /* Clear the child TID in user memory on exit */
#define CLONE_DETACHED       0x00400000 /* Unused, ignored */
#define CLONE_UNTRACED       0x00800000 /* Set if the tracing process can't force CLONE_PTRACE on this clone */
#define CLONE_CHILD_SETTID   0x01000000 /* Store the child TID in the child's memory */
#define CLONE_NEWCGROUP      0x02000000 /* New cgroup namespace */
#define CLONE_NEWUTS         0x04000000 /* New utsname namespace */
#define CLONE_NEWIPC         0x08000000 /* New ipc namespace */
#define CLONE_NEWUSER        0x10000000 /* New user namespace */
#define CLONE_NEWPID         0x20000000 /* New pid namespace */
#define CLONE_NEWNET         0x40000000 /* New network namespace */
#define CLONE_IO             0x80000000 /* Clone I/O context */

typedef enum { PROC_UNUSED = 0, PROC_READY, PROC_WAITING, PROC_ZOMBIE, PROC_EXITED } proc_state_t;

/* Thread group: shared process-level resources (tgid) */
typedef struct thread_group {
    int tgid;                           /* Thread group ID (PID of leader) */
    int ref_count;                      /* Number of threads referencing this group */
    int num_threads;                    /* Active thread count */
    int ppid;                           /* Parent PID of thread group */
    int pgid;                           /* Process group ID */
    int sid;                            /* Session ID */
    int exit_code;                      /* Exit status code of group */
    int is_stopped;                     /* 1 if stopped by SIGSTOP/SIGTSTP */
    int stopped_reported;
    int stop_sig;
    int term_sig;

    address_space_t as;                 /* Shared address space */
    open_file_t* fds[MAX_FDS];          /* Shared VFS open files table */
    uint8_t fd_flags[MAX_FDS];          /* Shared descriptor flags */
    char cwd[1024];                     /* Shared working directory */
    struct sigaction sig_actions[NSIG]; /* Shared signal actions */
    sigset_t sig_pending_group;         /* Signals pending on entire group */

    uint64_t heap_end;
    uint64_t heap_mapped_up_to;
    struct vma* vma_list;
    uint64_t aslr_mmap_offset;
    uint64_t aslr_stack_offset;
    uint64_t aslr_brk_offset;
    uint64_t aslr_pie_offset;

    int agent_id;
    uint32_t cpu_ticks_total;           /* Aggregated CPU ticks */
    uint32_t umask_val;
    char name[32];
    char exe_path[256];
    void* ctty;
    int in_use;
} thread_group_t;

/* Task / Thread: individual schedulable unit (tid) */
typedef struct process {
    thread_group_t* group;      /* Thread group pointer */
    int tid;                    /* Unique thread ID */
    int pid;                    /* Group leader PID (Linux getpid() == tgid) */
    int ppid;                   /* Group parent PID */
    int pgid;                   /* Process group ID */
    int sid;                    /* Session ID */

    address_space_t as;         /* Points to group->as for vmm_switch */
    uint64_t rsp;               /* saved kernel-mode rsp -- the switch_task continuation point */
    uint64_t kernel_stack_top;  /* becomes TSS.rsp0 whenever this process is current */
    uint8_t* kernel_stack_base; /* for freeing on exit */
    int      kstack_slot;       /* slot in dedicated kstack virtual region */
    uint64_t current_syscall;   /* active syscall number for fault reporting */
    uint64_t entry_point;       /* read by process_trampoline on this process's first run */
    uint64_t user_stack_top;
    proc_state_t state;
    int is_kernel_task;         /* process 0 (the shell/idle) runs in ring0, in the kernel's own address space */
    char name[32];

    int waiting_for_pid;        /* valid when state == PROC_WAITING */
    void* wait_channel;         /* sleep channel for blocked pipes/events */
    int exit_code;              /* thread exit status code */

    sigset_t sig_pending;       /* Per-thread pending signals mask */
    sigset_t sig_blocked;       /* Per-thread blocked signals mask */
    stack_t  sigaltstack;       /* Per-thread alternate signal stack */
    struct sigaction* sig_actions; /* Points to group->sig_actions */
    uint32_t alarm_ticks;       /* Remaining ticks until SIGALRM */
    int is_stopped;             /* 1 if stopped by SIGSTOP/SIGTSTP */
    int stopped_reported;       /* 1 if stop status reported via waitpid */
    int stop_sig;               /* Signal that stopped the process */
    int term_sig;               /* Signal that terminated the process, if any */

    open_file_t** fds;          /* Points to group->fds */
    uint8_t*      fd_flags;     /* Points to group->fd_flags */
    char*         cwd;          /* Points to group->cwd */

    int is_forked;              /* resumes via resume_saved_state(&saved_regs) */
    struct registers saved_regs;

    uint64_t heap_end;          /* current program break */
    uint64_t heap_mapped_up_to; /* how far the heap has actually been paged in */

    struct vma* vma_list;       /* sorted list of memory areas (VMAs) */
    uint64_t aslr_mmap_offset;  /* ASLR random slide for mmap allocations (17 bits) */
    uint64_t aslr_stack_offset; /* ASLR random slide for user stack top (16 bits) */
    uint64_t aslr_brk_offset;   /* ASLR random slide for heap/brk base (17 bits) */
    uint64_t aslr_pie_offset;   /* ASLR random slide for ET_DYN / PIE base (18 bits) */
    uint64_t fs_base;           /* Thread-local storage (FS base MSR) */
    uint64_t gs_base;           /* User GS base MSR */
    int*     clear_child_tid;   /* set_tid_address / exit_group child tid */
    void*    robust_list_head;  /* Robust futex list head */

    uint32_t sleep_deadline;    /* Tick deadline for sleep/nanosleep */
    struct process* sleep_next; /* Next pointer in timer sleep wait queue */
    uint32_t cpu_ticks;         /* Ticks consumed while running */
    int nice;                   /* Scheduling nice value (-20 to 19) */

    /* FPU / SSE state area (512 bytes + 64 bytes alignment pad) */
    uint8_t fpu_buffer[512 + 64];

#define MAX_POLL_CHANNELS 64
    void* poll_channels[MAX_POLL_CHANNELS];
    int   num_poll_channels;
    int   current_op_flags;     /* Per-process active VFS operation flags */
    void* ctty;                 /* Controlling terminal (tty_t*) */
    int   agent_id;             /* Bound agent ID (0 if unconfined) */
    int   in_interrupt;         /* ISR nesting counter (0 if normal process context) */
    char  exe_path[256];        /* Executable path for /proc/self/exe */
    uint32_t umask_val;         /* File mode creation mask */
    uint64_t logged_unimpl_syscalls[8]; /* Bitmap of logged unhandled syscall numbers (0..511) */
} process_t;

void process_init_table(int use_kshell);
process_t* process_get_shell(void);
process_t* process_table_entry(int i);
process_t* process_find_by_pid(int pid);
process_t* process_find_by_tid(int tid);
thread_group_t* thread_group_alloc(void);
void thread_group_free(thread_group_t* tg);
void kernel_idle_task(void);
process_t* process_spawn_by_name(const char* name, int parent_pid);
process_t* process_spawn_from_elf(const char* name, const uint8_t* image, uint64_t image_size, int parent_pid, vnode_t* vn);
void process_destroy(process_t* p);
process_t* process_fork(process_t* parent, const struct registers* parent_regs);
int process_clone(process_t* parent, unsigned long clone_flags, void* child_stack, int* ptid, int* ctid, unsigned long newtls, const struct registers* parent_regs, process_t** out_child);

#endif
