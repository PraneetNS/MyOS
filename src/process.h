#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vmm.h"
#include "fs.h"
#include "idt.h" /* struct registers -- forked children resume from a saved snapshot of it */

#define MAX_PROCESSES 64
#define PROC_KERNEL_STACK_SIZE 16384
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

typedef enum { PROC_UNUSED = 0, PROC_READY, PROC_WAITING, PROC_ZOMBIE, PROC_EXITED } proc_state_t;

typedef struct process {
    address_space_t as;
    uint64_t rsp;               /* saved kernel-mode rsp -- the switch_task continuation point */
    uint64_t kernel_stack_top;  /* becomes TSS.rsp0 whenever this process is current */
    uint8_t* kernel_stack_base; /* for freeing on exit */
    uint64_t entry_point;       /* read by process_trampoline on this process's first run */
    uint64_t user_stack_top;
    proc_state_t state;
    int is_kernel_task;         /* process 0 (the shell/idle) runs in ring0, in the kernel's own address space */
    char name[32];

    int pid;
    int ppid;
    int pgid;                   /* Process group ID */
    int sid;                    /* Session ID */
    int waiting_for_pid;        /* valid when state == PROC_WAITING */
    void* wait_channel;         /* sleep channel for blocked pipes/events */
    int exit_code;              /* exit status code */

    sigset_t sig_pending;       /* Pending signals mask */
    sigset_t sig_blocked;       /* Blocked signals mask */
    struct sigaction sig_actions[NSIG]; /* Signal handlers */
    uint32_t alarm_ticks;       /* Remaining ticks until SIGALRM */
    int is_stopped;             /* 1 if stopped by SIGSTOP/SIGTSTP */
    int stopped_reported;       /* 1 if stop status reported via waitpid */
    int stop_sig;               /* Signal that stopped the process */
    int term_sig;               /* Signal that terminated the process, if any */

    open_file_t* fds[MAX_FDS];  /* VFS open_file pointers, refcounted */
    uint8_t fd_flags[MAX_FDS];  /* descriptor flags (FD_CLOEXEC) */
    char cwd[1024];             /* Current working directory (PATH_MAX) */

    int is_forked;              /* resumes via resume_saved_state(&saved_regs) */
    struct registers saved_regs;

    uint64_t heap_end;          /* current program break, grows from HEAP_BASE via sys_sbrk() */
    uint64_t heap_mapped_up_to; /* how far the heap has actually been paged in */

    struct vma* vma_list;       /* sorted list of memory areas (VMAs) */
    uint64_t aslr_mmap_offset;  /* ASLR random slide for mmap allocations (17 bits) */
    uint64_t aslr_stack_offset; /* ASLR random slide for user stack top (16 bits) */
    uint64_t aslr_brk_offset;   /* ASLR random slide for heap/brk base (17 bits) */
    uint64_t aslr_pie_offset;   /* ASLR random slide for ET_DYN / PIE base (18 bits) */
    uint64_t fs_base;           /* Thread-local storage (FS base MSR) */
    uint64_t gs_base;           /* User GS base MSR */
    int*     clear_child_tid;   /* set_tid_address / exit_group child tid */

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
    char  exe_path[256];        /* Executable path for /proc/self/exe */
    uint32_t umask_val;         /* File mode creation mask */
    uint64_t logged_unimpl_syscalls[8]; /* Bitmap of logged unhandled syscall numbers (0..511) */
} process_t;

void process_init_table(int use_kshell);
process_t* process_get_shell(void);
process_t* process_table_entry(int i);
process_t* process_find_by_pid(int pid);
void kernel_idle_task(void);
process_t* process_spawn_by_name(const char* name, int parent_pid);
process_t* process_spawn_from_elf(const char* name, const uint8_t* image, uint64_t image_size, int parent_pid, vnode_t* vn);
void process_destroy(process_t* p);
process_t* process_fork(process_t* parent, const struct registers* parent_regs);

#endif
