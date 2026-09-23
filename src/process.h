#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vmm.h"
#include "fs.h"
#include "idt.h" /* struct registers -- forked children resume from a saved snapshot of it */

#define MAX_PROCESSES 4
#define PROC_KERNEL_STACK_SIZE 8192
#define MAX_FDS 4
#define HEAP_BASE 0x900000u      /* 9MB -- clear of code at 0x800000 and the 0xC0000000 stack region */
#define HEAP_MAX  (HEAP_BASE + 0x100000u) /* cap growth at 1MB per process */

typedef enum { PROC_UNUSED = 0, PROC_READY, PROC_WAITING, PROC_EXITED } proc_state_t;

typedef struct {
    const fs_entry_t* entry;
    uint32_t offset;
    int in_use;
} fd_entry_t;

typedef struct process {
    address_space_t as;
    uint32_t esp;               /* saved kernel-mode esp -- the switch_task continuation point */
    uint32_t kernel_stack_top;  /* becomes TSS.esp0 whenever this process is current */
    uint8_t* kernel_stack_base; /* for freeing on exit */
    uint32_t entry_point;       /* read by process_trampoline on this process's first run */
    uint32_t user_stack_top;
    proc_state_t state;
    int is_kernel_task;         /* process 0 (the shell) runs in ring0, in the kernel's own address space */
    char name[32];

    int pid;
    int ppid;
    int waiting_for_pid;        /* valid when state == PROC_WAITING */

    fd_entry_t fds[MAX_FDS];    /* Stage 8: per-process open file table, backed by fs.c */

    int is_forked;              /* Stage 9: resumes via resume_saved_state(&saved_regs), not process_trampoline */
    struct registers saved_regs;

    uint32_t heap_end;          /* Stage 11: current program break, grows from HEAP_BASE via sys_sbrk() */
    uint32_t heap_mapped_up_to; /* how far the heap has actually been paged in (<= heap_end, rounded to a page) */
} process_t;

/* Sets up the fixed process table with slot 0 as the always-resident
   shell (ring0, kernel address space, entry = shell_run, pid 0). Call
   once, before scheduler_start(). */
void process_init_table(void);

process_t* process_get_shell(void);
process_t* process_table_entry(int i);
process_t* process_find_by_pid(int pid);

/* Loads an ELF into a fresh process slot + fresh address space and
   marks it READY to run. `parent_pid` becomes the new process's ppid.
   Returns NULL if no free slot or the load failed. Does NOT start
   running it -- the scheduler picks it up on its next switch. */
process_t* process_spawn_from_elf(const char* name, const uint8_t* image, uint32_t image_size, int parent_pid);

/* Frees a process's address space and kernel stack, marking its slot
   free for reuse. */
void process_destroy(process_t* p);

/* Duplicates `parent` (its address space via vmm_clone_user_pages, and
   its exact CPU register state at the moment of the fork syscall) into
   a fresh process slot. The child resumes at the SAME instruction the
   parent was at, with eax forced to 0 (classic fork() semantics: the
   syscall "returns twice"). Returns NULL on failure (no free slot, out
   of memory, etc). */
process_t* process_fork(process_t* parent, const struct registers* parent_regs);

#endif
