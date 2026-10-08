#include "process.h"
#include "vma.h"
#include "pmm.h"
#include "kheap.h"
#include "vga.h"
#include "elf.h"
#include "paging.h"
#include "shell.h"
#include "usermode.h"
#include "scheduler.h"
#include "serial.h"
#include "keyboard.h"
#include "timer.h"
#include "fpu.h"

int respawn_shell_needed = 0;

void kernel_idle_task(void) {
    for (;;) {
        while (serial_received()) {
            char c = serial_read();
            keyboard_handle_char(c);
        }
        if (respawn_shell_needed) {
            int old_pid = respawn_shell_needed;
            respawn_shell_needed = 0;
            process_t* old_sh = process_find_by_pid(old_pid);
            if (old_sh) {
                process_destroy(old_sh);
            }
            kprintf("[init] shell exited, respawning sh.elf...\n");
            process_t* sh = process_spawn_by_name("sh.elf", 0);
            if (sh) {
                kprintf("[init] respawned sh.elf (PID %d)\n", sh->pid);
            }
        }
        scheduler_yield();
        asm volatile ("sti; hlt");
    }
}

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

static void init_fds(process_t* p) {
    p->fds[0] = vfs_get_console_stdin();
    p->fds[1] = vfs_get_console_stdout();
    p->fds[2] = vfs_get_console_stderr();

    for (int i = 0; i < MAX_FDS; i++) {
        p->fd_flags[i] = 0;
    }

    for (int i = 3; i < MAX_FDS; i++) {
        p->fds[i] = 0;
    }

    p->cwd[0] = '/';
    p->cwd[1] = '\0';
}

void process_init_table(int use_kshell) {
    for (int i = 0; i < MAX_PROCESSES; i++) table[i].state = PROC_UNUSED;

    process_t* shell = &table[0];
    shell->as.directory      = boot_page_directory;
    shell->as.directory_phys = paging_get_kernel_dir_phys();

    shell->kernel_stack_base = (uint8_t*) kmalloc(PROC_KERNEL_STACK_SIZE);
    shell->kernel_stack_top  = (uint32_t)(shell->kernel_stack_base + PROC_KERNEL_STACK_SIZE);
    shell->is_kernel_task = 1;
    shell->state = PROC_READY;
    shell->pid = 0;
    shell->ppid = -1;
    shell->pgid = 0;
    shell->sid = 0;
    shell->waiting_for_pid = -999;
    shell->wait_channel = 0;
    shell->exit_code = 0;
    shell->is_forked = 0;
    shell->heap_end = HEAP_BASE;
    shell->heap_mapped_up_to = HEAP_BASE;
    shell->vma_list = NULL;
    shell->sleep_deadline = 0;
    shell->sleep_next = 0;
    shell->cpu_ticks = 0;
    shell->nice = 0;
    init_fds(shell);
    signal_init_proc(shell);

    if (use_kshell) {
        const char* n = "kshell";
        int i = 0; for (; n[i] && i < 31; i++) shell->name[i] = n[i]; shell->name[i] = '\0';
        fabricate_initial_frame(shell, shell_run);
    } else {
        const char* n = "idle";
        int i = 0; for (; n[i] && i < 31; i++) shell->name[i] = n[i]; shell->name[i] = '\0';
        fabricate_initial_frame(shell, kernel_idle_task);
    }
}

process_t* process_spawn_by_name(const char* name, int parent_pid) {
    vnode_t* vn = 0;
    if (vfs_resolve_path(name, "/", &vn) != 0 || !vn) {
        char bin_path[64];
        bin_path[0] = '/';
        int l = 0;
        while (name[l] && l < 60) {
            bin_path[l + 1] = name[l];
            l++;
        }
        bin_path[l + 1] = '\0';
        if (vfs_resolve_path(bin_path, "/", &vn) != 0 || !vn) {
            bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
            l = 0;
            while (name[l] && l < 50) {
                bin_path[5 + l] = name[l];
                l++;
            }
            bin_path[5 + l] = '\0';
            if (vfs_resolve_path(bin_path, "/", &vn) != 0 || !vn) {
                return 0;
            }
        }
    }

    uint32_t size = vn->size;
    if (size == 0) { vnode_unref(vn); return 0; }

    uint32_t alloc_size = ((size + 511) / 512) * 512;
    uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
    if (!buf) { vnode_unref(vn); return 0; }

    int n = vn->ops->read(vn, 0, buf, size);
    if (n <= 0) { vnode_unref(vn); kfree(buf); return 0; }

    process_t* child = process_spawn_from_elf(name, buf, (uint32_t) n, parent_pid, vn);
    vnode_unref(vn);
    kfree(buf);
    return child;
}

process_t* process_get_shell(void) { return &table[0]; }
process_t* process_table_entry(int i) { return &table[i]; }

process_t* process_find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (table[i].state != PROC_UNUSED && table[i].pid == pid)
            return &table[i];
    return 0;
}

process_t* process_spawn_from_elf(const char* name, const uint8_t* image, uint32_t image_size, int parent_pid, vnode_t* vn) {
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) { /* slot 0 is always the shell */
        if (table[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        kprintf("[proc] no free process slots (max %d)\n", MAX_PROCESSES);
        return 0;
    }

    process_t* p = &table[slot];
    p->as = vmm_create_address_space();
    if (!p->as.directory) return 0;

    uint32_t entry, stack_top;
    const char* argv[2] = { name, 0 };
    vma_t* vmas = NULL;
    if (elf_load_into(image, image_size, vn, &p->as, 1, argv, &entry, &stack_top, &vmas) != 0) {
        vmm_destroy_address_space(&p->as);
        return 0;
    }
    p->entry_point = entry;
    p->user_stack_top = stack_top;
    p->vma_list = vmas;

    p->kernel_stack_base = (uint8_t*) kmalloc(PROC_KERNEL_STACK_SIZE);
    p->kernel_stack_top  = (uint32_t)(p->kernel_stack_base + PROC_KERNEL_STACK_SIZE);
    p->is_kernel_task = 0;
    p->pid = next_pid++;
    p->ppid = parent_pid;
    if (parent_pid > 0) {
        process_t* parent = process_find_by_pid(parent_pid);
        if (parent) {
            p->pgid = parent->pgid;
            p->sid = parent->sid;
        } else {
            p->pgid = p->pid;
            p->sid = p->pid;
        }
    } else {
        p->pgid = p->pid;
        p->sid = p->pid;
    }
    p->waiting_for_pid = -999;
    p->wait_channel = 0;
    p->exit_code = 0;
    p->is_forked = 0;
    p->sleep_deadline = 0;
    p->sleep_next = 0;
    p->cpu_ticks = 0;
    p->nice = (parent_pid > 0 && process_find_by_pid(parent_pid)) ? process_find_by_pid(parent_pid)->nice : 0;
    p->heap_end = HEAP_BASE;
    p->heap_mapped_up_to = HEAP_BASE;
    init_fds(p);
    signal_init_proc(p);
    fpu_init_proc(p);

    int i = 0; for (; name[i] && i < 31; i++) p->name[i] = name[i]; p->name[i] = '\0';

    fabricate_initial_frame(p, process_trampoline);
    p->state = PROC_READY;

    return p;
}

void process_destroy(process_t* p) {
    if (p->state == PROC_UNUSED) return;
    timer_sleep_dequeue(p);
    p->sleep_deadline = 0;
    p->sleep_next = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        p->fd_flags[i] = 0;
        if (p->fds[i]) {
            open_file_unref(p->fds[i]);
            p->fds[i] = 0;
        }
    }
    if (p->vma_list) {
        vma_free_list(p->vma_list);
        p->vma_list = NULL;
    }
    if (!p->is_kernel_task)
        vmm_destroy_address_space(&p->as);
    if (p->kernel_stack_base) { kfree(p->kernel_stack_base); p->kernel_stack_base = 0; }
    p->waiting_for_pid = -999;
    p->wait_channel = 0;
    p->exit_code = 0;
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

    p->vma_list = vma_clone_list(parent->vma_list);

    p->kernel_stack_base = (uint8_t*) kmalloc(PROC_KERNEL_STACK_SIZE);
    p->kernel_stack_top  = (uint32_t)(p->kernel_stack_base + PROC_KERNEL_STACK_SIZE);
    p->is_kernel_task = 0;
    p->pid = next_pid++;
    p->ppid = parent->pid;
    p->pgid = parent->pgid;
    p->sid = parent->sid;
    p->waiting_for_pid = -999;
    p->wait_channel = 0;
    p->sleep_deadline = 0;
    p->sleep_next = 0;
    p->cpu_ticks = 0;
    p->nice = parent->nice;
    p->exit_code = 0;
    p->sig_pending = 0;
    p->sig_blocked = parent->sig_blocked;
    p->alarm_ticks = 0;
    p->is_stopped = 0;
    p->stopped_reported = 0;
    p->stop_sig = 0;
    p->term_sig = 0;
    for (int k = 0; k < NSIG; k++) {
        p->sig_actions[k] = parent->sig_actions[k];
    }

    p->heap_end = parent->heap_end;
    p->heap_mapped_up_to = parent->heap_mapped_up_to;

    fpu_save(parent);
    fpu_copy(p, parent);

    /* Inherit open files with shared reference counts and copy descriptor flags */
    for (int i = 0; i < MAX_FDS; i++) {
        p->fds[i] = parent->fds[i];
        p->fd_flags[i] = parent->fd_flags[i];
        if (p->fds[i]) {
            open_file_ref(p->fds[i]);
        }
    }

    int c = 0;
    while (parent->cwd[c] && c < 63) {
        p->cwd[c] = parent->cwd[c];
        c++;
    }
    p->cwd[c] = '\0';

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
