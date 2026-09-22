#include "process.h"
#include "pmm.h"
#include "kheap.h"
#include "vga.h"
#include "elf.h"
#include "paging.h"
#include "shell.h"
#include "usermode.h"
#include "scheduler.h"

static process_t table[MAX_PROCESSES];
static int next_pid = 1; /* pid 0 is reserved for the shell */

/* The "return address" fabricated onto a brand-new process's kernel
   stack (see the switch_task frame below). It runs once, on that
   process's very first scheduler switch-in, with CR3 and TSS.esp0
   already pointing at this process (the scheduler sets those before
   every switch, first run or not) -- so all it has to do is enter
   ring 3. Every switch-in AFTER this one resumes mid-suspension via a
   plain switch_task `ret`, the same mechanism Stage 4 used for kernel
   threads; this trampoline only ever runs once per process. */
extern void resume_saved_state(struct registers* r); /* boot/resume_state.s */

/* The "return address" fabricated onto a brand-new process's kernel
   stack (see the switch_task frame below). It runs once, on that
   process's very first scheduler switch-in, with CR3 and TSS.esp0
   already pointing at this process (the scheduler sets those before
   every switch, first run or not) -- so all it has to do is enter
   ring 3, either fresh (process_spawn_from_elf) or by resuming a
   forked snapshot (process_fork). Every switch-in AFTER this one
   resumes mid-suspension via a plain switch_task `ret`, the same
   mechanism Stage 4 used for kernel threads; this trampoline only ever
   runs once per process. */
static void process_trampoline(void) {
    process_t* p = scheduler_current();
    if (p->is_forked)
        resume_saved_state(&p->saved_regs); /* never returns */
    else
        enter_usermode(p->entry_point, p->user_stack_top); /* never returns */
}

static void fabricate_initial_frame(process_t* p, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)(p->kernel_stack_top);
    *(--sp) = (uint32_t) entry;
    *(--sp) = 0; /* ebp */
    *(--sp) = 0; /* ebx */
    *(--sp) = 0; /* esi */
    *(--sp) = 0; /* edi */
    *(--sp) = 0x202; /* eflags, IF set */
    p->esp = (uint32_t) sp;
}

static void clear_fds(process_t* p) {
    for (int i = 0; i < MAX_FDS; i++) p->fds[i].in_use = 0;
}

void process_init_table(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) table[i].state = PROC_UNUSED;

    process_t* shell = &table[0];
    shell->as.directory      = (uint32_t*) paging_get_kernel_dir_phys();
    shell->as.directory_phys = paging_get_kernel_dir_phys();
    shell->as.owned_count    = 0; /* the shell doesn't own this address space -- never destroyed */

    shell->kernel_stack_base = (uint8_t*) kmalloc(PROC_KERNEL_STACK_SIZE);
    shell->kernel_stack_top  = (uint32_t)(shell->kernel_stack_base + PROC_KERNEL_STACK_SIZE);
    shell->is_kernel_task = 1;
    shell->state = PROC_READY;
    shell->pid = 0;
    shell->ppid = -1;
    shell->waiting_for_pid = -1;
    shell->is_forked = 0;
    clear_fds(shell);

    const char* n = "shell";
    int i = 0; for (; n[i] && i < 31; i++) shell->name[i] = n[i]; shell->name[i] = '\0';

    /* The shell runs in ring0, so its first "switch-in" should just
       call shell_run() directly -- no trampoline/ring3 transition
       needed. shell_run() matches the required void(void) signature
       exactly. */
    fabricate_initial_frame(shell, shell_run);
}

process_t* process_get_shell(void) { return &table[0]; }
process_t* process_table_entry(int i) { return &table[i]; }

process_t* process_find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (table[i].state != PROC_UNUSED && table[i].pid == pid)
            return &table[i];
    return 0;
}

process_t* process_spawn_from_elf(const char* name, const uint8_t* image, uint32_t image_size, int parent_pid) {
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) { /* slot 0 is always the shell */
        if (table[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        terminal_writestring("[proc] no free process slots (max ");
        terminal_putchar('0' + MAX_PROCESSES);
        terminal_writestring(")\n");
        return 0;
    }

    process_t* p = &table[slot];
    p->as = vmm_create_address_space();
    if (!p->as.directory) return 0;

    uint32_t entry, stack_top;
    if (elf_load_into(image, image_size, &p->as, &entry, &stack_top) != 0) {
        vmm_destroy_address_space(&p->as);
        return 0;
    }
    p->entry_point = entry;
    p->user_stack_top = stack_top;

    p->kernel_stack_base = (uint8_t*) kmalloc(PROC_KERNEL_STACK_SIZE);
    p->kernel_stack_top  = (uint32_t)(p->kernel_stack_base + PROC_KERNEL_STACK_SIZE);
    p->is_kernel_task = 0;
    p->pid = next_pid++;
    p->ppid = parent_pid;
    p->waiting_for_pid = -1;
    p->is_forked = 0;
    clear_fds(p);

    int i = 0; for (; name[i] && i < 31; i++) p->name[i] = name[i]; p->name[i] = '\0';

    fabricate_initial_frame(p, process_trampoline);
    p->state = PROC_READY;

    return p;
}

void process_destroy(process_t* p) {
    if (p->state == PROC_UNUSED) return;
    if (!p->is_kernel_task)
        vmm_destroy_address_space(&p->as);
    if (p->kernel_stack_base) { kfree(p->kernel_stack_base); p->kernel_stack_base = 0; }
    p->state = PROC_UNUSED;
}

process_t* process_fork(process_t* parent, const struct registers* parent_regs) {
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (table[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        terminal_writestring("[proc] fork: no free process slots\n");
        return 0;
    }

    process_t* p = &table[slot];
    p->as = vmm_create_address_space();
    if (!p->as.directory) return 0;

    if (vmm_clone_user_pages(&p->as, &parent->as) != 0) {
        terminal_writestring("[proc] fork: failed to clone address space (out of memory?)\n");
        vmm_destroy_address_space(&p->as);
        return 0;
    }

    p->kernel_stack_base = (uint8_t*) kmalloc(PROC_KERNEL_STACK_SIZE);
    p->kernel_stack_top  = (uint32_t)(p->kernel_stack_base + PROC_KERNEL_STACK_SIZE);
    p->is_kernel_task = 0;
    p->pid = next_pid++;
    p->ppid = parent->pid;
    p->waiting_for_pid = -1;

    /* Inherit open file descriptors -- real fork() semantics share the
       underlying file position between parent and child; ours just
       copies it (a simplification: they'll diverge independently on
       further reads instead of sharing one offset, documented as a
       known limitation). */
    for (int i = 0; i < MAX_FDS; i++) p->fds[i] = parent->fds[i];

    int i = 0; for (; parent->name[i] && i < 26; i++) p->name[i] = parent->name[i];
    p->name[i] = 0;
    const char* suffix = "-fork";
    for (int j = 0; suffix[j] && i < 31; j++, i++) p->name[i] = suffix[j];
    p->name[i] = '\0';

    /* Snapshot the parent's exact CPU state at the fork syscall, with
       eax forced to 0 -- when this child is first scheduled in, its
       trampoline will resume EXACTLY here, and to its own code it will
       look just like fork() returned 0, while the parent (still running
       normally) sees the child's pid instead (set by syscall.c). */
    p->is_forked = 1;
    p->saved_regs = *parent_regs;
    p->saved_regs.eax = 0;

    fabricate_initial_frame(p, process_trampoline);
    p->state = PROC_READY;

    return p;
}
