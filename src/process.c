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
#include "kmutex.h"
#include "tty.h"
#include "agent.h"
#include "random.h"
#include "kstack.h"
#include "uaccess.h"
#include "futex.h"

int respawn_shell_needed = 0;

void kernel_idle_task(void) {
    for (;;) {
        agent_killswitch_check_deferred();
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
static thread_group_t thread_group_table[MAX_PROCESSES];
static int next_pid = 1; /* pid 0 is reserved for the shell */

thread_group_t* thread_group_alloc(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!thread_group_table[i].in_use) {
            memset(&thread_group_table[i], 0, sizeof(thread_group_t));
            thread_group_table[i].in_use = 1;
            return &thread_group_table[i];
        }
    }
    return NULL;
}

void thread_group_free(thread_group_t* tg) {
    if (!tg || !tg->in_use) return;
    for (int i = 0; i < MAX_FDS; i++) {
        tg->fd_flags[i] = 0;
        if (tg->fds[i]) {
            open_file_unref(tg->fds[i]);
            tg->fds[i] = 0;
        }
    }
    if (tg->vma_list) {
        vma_free_list(tg->vma_list);
        tg->vma_list = NULL;
    }
    if (tg->as.pml4 && tg->tgid != 0) {
        vmm_destroy_address_space(&tg->as);
        tg->as.pml4 = 0;
    }
    tg->in_use = 0;
}

/* The "return address" fabricated onto a brand-new process's kernel
   stack (see the switch_task frame below). It runs once, on that
   process's very first scheduler switch-in, with CR3 and TSS.esp0
   already pointing at this process (the scheduler sets those before
   every switch, first run or not) -- so all it has to do is enter
   ring 3. Every switch-in AFTER this one resumes mid-suspension via a
   plain switch_task `ret`, the same mechanism Stage 4 used for kernel
   threads; this trampoline only ever runs once per process. */
extern void resume_saved_state(struct registers* r); /* boot/resume_state.s */

static void process_trampoline(void) {
    process_t* p = scheduler_current();
    if (p->is_forked)
        resume_saved_state(&p->saved_regs); /* never returns */
    else
        enter_usermode(p->entry_point, p->user_stack_top); /* never returns */
}

static void fabricate_initial_frame(process_t* p, void (*entry)(void)) {
    uint64_t* sp = (uint64_t*)(p->kernel_stack_top);
    *(--sp) = (uint64_t) entry;
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */
    *(--sp) = 0x202; /* rflags: IF set */
    p->rsp = (uint64_t) sp;
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
    for (int i = 0; i < MAX_PROCESSES; i++) {
        table[i].state = PROC_UNUSED;
        thread_group_table[i].in_use = 0;
    }

    thread_group_t* tg0 = &thread_group_table[0];
    memset(tg0, 0, sizeof(thread_group_t));
    tg0->in_use = 1;
    tg0->tgid = 0;
    tg0->ref_count = 1;
    tg0->num_threads = 1;
    tg0->as.pml4      = paging_get_kernel_pml4_virt();
    tg0->as.pml4_phys = paging_get_kernel_pml4_phys();

    process_t* shell = &table[0];
    shell->group = tg0;
    shell->as = tg0->as;
    shell->fds = tg0->fds;
    shell->fd_flags = tg0->fd_flags;
    shell->cwd = tg0->cwd;
    shell->sig_actions = tg0->sig_actions;

    kstack_init();
    shell->kstack_slot = 0;
    if (kstack_alloc(0, &shell->kernel_stack_base, &shell->kernel_stack_top) != 0) {
        serial_printf("[process] Failed to allocate kstack for shell\n");
    }
    shell->is_kernel_task = 1;
    shell->state = PROC_READY;
    shell->pid = 0;
    shell->tid = 0;
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
    shell->clear_child_tid = 0;
    shell->robust_list_head = 0;
    memset(&shell->sigaltstack, 0, sizeof(shell->sigaltstack));
    shell->sleep_deadline = 0;
    shell->sleep_next = 0;
    shell->cpu_ticks = 0;
    shell->nice = 0;
    shell->current_op_flags = 0;
    shell->agent_id = 0;
    shell->exe_path[0] = '/'; shell->exe_path[1] = '\0';
    shell->umask_val = 0022;
    for (int u = 0; u < 8; u++) shell->logged_unimpl_syscalls[u] = 0;
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
    char resolved_path[256];
    resolved_path[0] = '\0';
    if (vfs_resolve_path(name, "/", &vn) == 0 && vn) {
        vfs_path_canonical(name, "/", resolved_path, sizeof(resolved_path));
    } else {
        char bin_path[64];
        bin_path[0] = '/';
        int l = 0;
        while (name[l] && l < 60) {
            bin_path[l + 1] = name[l];
            l++;
        }
        bin_path[l + 1] = '\0';
        if (vfs_resolve_path(bin_path, "/", &vn) == 0 && vn) {
            vfs_path_canonical(bin_path, "/", resolved_path, sizeof(resolved_path));
        } else {
            bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
            l = 0;
            while (name[l] && l < 50) {
                bin_path[5 + l] = name[l];
                l++;
            }
            bin_path[5 + l] = '\0';
            if (vfs_resolve_path(bin_path, "/", &vn) == 0 && vn) {
                vfs_path_canonical(bin_path, "/", resolved_path, sizeof(resolved_path));
            } else {
                bin_path[0] = '/'; bin_path[1] = 's'; bin_path[2] = 'b'; bin_path[3] = 'i'; bin_path[4] = 'n'; bin_path[5] = '/';
                l = 0;
                while (name[l] && l < 50) {
                    bin_path[6 + l] = name[l];
                    l++;
                }
                bin_path[6 + l] = '\0';
                if (vfs_resolve_path(bin_path, "/", &vn) == 0 && vn) {
                    vfs_path_canonical(bin_path, "/", resolved_path, sizeof(resolved_path));
                } else {
                    return 0;
                }
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
    if (child && resolved_path[0]) {
        int k = 0;
        while (resolved_path[k] && k < 255) { child->exe_path[k] = resolved_path[k]; k++; }
        child->exe_path[k] = '\0';
    }
    vnode_unref(vn);
    kfree(buf);
    return child;
}

process_t* process_get_shell(void) { return &table[0]; }
process_t* process_table_entry(int i) { return &table[i]; }

process_t* process_find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (table[i].state != PROC_UNUSED && table[i].pid == pid && table[i].tid == pid)
            return &table[i];
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (table[i].state != PROC_UNUSED && table[i].pid == pid)
            return &table[i];
    return 0;
}

process_t* process_find_by_tid(int tid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (table[i].state != PROC_UNUSED && table[i].tid == tid)
            return &table[i];
    return 0;
}

process_t* process_spawn_from_elf(const char* name, const uint8_t* image, uint64_t image_size, int parent_pid, vnode_t* vn) {
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) { /* slot 0 is always the shell */
        if (table[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        kprintf("[proc] no free process slots (max %d)\n", MAX_PROCESSES);
        return 0;
    }

    process_t* p = &table[slot];
    p->in_interrupt = 0;
    p->as = vmm_create_address_space();
    if (!p->as.pml4) return 0;

    uint64_t entry, stack_top;
    const char* argv[2] = { name, 0 };
    const char* default_env[5] = {
        "HOME=/",
        "PWD=/",
        "USER=root",
        "PATH=/bin:/",
        0
    };
    p->aslr_mmap_offset  = ((uint64_t)(random_u32() & 0x1FFFFULL)) << 12;
    p->aslr_stack_offset = (((uint64_t)(random_u32() & 0x7FFULL)) << 12) | (((uint64_t)(random_u32() & 0x1FULL)) << 4);
    p->aslr_brk_offset   = ((uint64_t)(random_u32() & 0x1FFFFULL)) << 12;
    p->aslr_pie_offset   = ((uint64_t)(random_u32() & 0x3FFFFULL)) << 12;
    p->fs_base = 0;
    p->gs_base = 0;
    p->clear_child_tid = 0;
    p->robust_list_head = 0;
    memset(&p->sigaltstack, 0, sizeof(p->sigaltstack));

    vma_t* vmas = NULL;
    if (elf_load_into(image, image_size, vn, p, &p->as, 1, argv, 4, default_env, &entry, &stack_top, &vmas) != 0) {
        vmm_destroy_address_space(&p->as);
        return 0;
    }
    p->entry_point = entry;
    p->user_stack_top = stack_top;
    p->vma_list = vmas;

    thread_group_t* tg = thread_group_alloc();
    if (!tg) {
        vmm_destroy_address_space(&p->as);
        return 0;
    }

    p->kstack_slot = slot;
    if (kstack_alloc(slot, &p->kernel_stack_base, &p->kernel_stack_top) != 0) {
        vmm_destroy_address_space(&p->as);
        thread_group_free(tg);
        return 0;
    }

    tg->tgid = next_pid++;
    tg->ref_count = 1;
    tg->num_threads = 1;
    tg->as = p->as;
    tg->vma_list = p->vma_list;
    tg->heap_end = HEAP_BASE + p->aslr_brk_offset;
    tg->heap_mapped_up_to = tg->heap_end;
    tg->ppid = parent_pid;
    tg->agent_id = (parent_pid > 0 && process_find_by_pid(parent_pid)) ? process_find_by_pid(parent_pid)->agent_id : 0;

    p->group = tg;
    p->pid = tg->tgid;
    p->tid = tg->tgid;
    p->ppid = parent_pid;
    p->fds = tg->fds;
    p->fd_flags = tg->fd_flags;
    p->cwd = tg->cwd;
    p->sig_actions = tg->sig_actions;
    p->is_kernel_task = 0;

    if (parent_pid > 0) {
        process_t* parent = process_find_by_pid(parent_pid);
        if (parent) {
            p->pgid = parent->pgid;
            p->sid = parent->sid;
            p->ctty = parent->ctty;
        } else {
            p->pgid = p->pid;
            p->sid = p->pid;
            p->ctty = NULL;
        }
    } else {
        p->pgid = p->pid;
        p->sid = p->pid;
        p->ctty = tty_get_global();
    }
    p->waiting_for_pid = -999;
    p->wait_channel = 0;
    p->exit_code = 0;
    p->is_forked = 0;
    p->sleep_deadline = 0;
    p->sleep_next = 0;
    p->cpu_ticks = 0;
    p->nice = (parent_pid > 0 && process_find_by_pid(parent_pid)) ? process_find_by_pid(parent_pid)->nice : 0;
    p->heap_end = tg->heap_end;
    p->heap_mapped_up_to = tg->heap_mapped_up_to;
    p->current_op_flags = 0;
    p->agent_id = tg->agent_id;
    if (p->agent_id > 0) {
        agent_t* ag = agent_get_by_id(p->agent_id);
        if (ag) ag->usage.procs_count++;
    }
    init_fds(p);
    signal_init_proc(p);
    fpu_init_proc(p);

    int i = 0; for (; name[i] && i < 31; i++) p->name[i] = name[i]; p->name[i] = '\0';
    vfs_path_canonical(name, "/", p->exe_path, sizeof(p->exe_path));
    p->umask_val = 0022;
    for (int u = 0; u < 8; u++) p->logged_unimpl_syscalls[u] = 0;

    fabricate_initial_frame(p, process_trampoline);
    p->state = PROC_READY;

    return p;
}

void process_destroy(process_t* p) {
    if (p->state == PROC_UNUSED) return;
    kmutex_assert_none_held();
    if (p->agent_id > 0) {
        if (p->group && p->group->num_threads > 1 && p->tid != p->pid) {
            agent_thread_exited(p->agent_id);
        } else {
            agent_proc_exited(p->agent_id);
        }
        p->agent_id = 0;
    }
    timer_sleep_dequeue(p);
    p->sleep_deadline = 0;
    p->sleep_next = 0;

    if (p->group) {
        if (p->tid == p->pid) {
            for (int i = 1; i < MAX_PROCESSES; i++) {
                if (&table[i] != p && table[i].group == p->group && table[i].state == PROC_ZOMBIE) {
                    process_destroy(&table[i]);
                }
            }
        }
        p->group->ref_count--;
        if (p->group->ref_count <= 0) {
            thread_group_free(p->group);
        }
        p->group = NULL;
    }

    if (p->kernel_stack_base) {
        kstack_free(p->kstack_slot);
        p->kernel_stack_base = 0;
        p->kernel_stack_top = 0;
    }
    p->waiting_for_pid = -999;
    p->wait_channel = 0;
    p->exit_code = 0;
    p->in_interrupt = 0;
    p->state = PROC_UNUSED;
}

process_t* process_fork(process_t* parent, const struct registers* parent_regs) {
    if (parent->agent_id > 0) {
        if (!agent_can_fork(parent->agent_id)) {
            return 0;
        }
    }

    /* Reap any zombie secondary threads in this group */
    if (parent->group) {
        for (int i = 1; i < MAX_PROCESSES; i++) {
            if (&table[i] != scheduler_current() && table[i].group == parent->group && table[i].tid != table[i].pid && table[i].state == PROC_ZOMBIE) {
                process_destroy(&table[i]);
            }
        }
    }

    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (table[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        terminal_writestring("[proc] fork: no free process slots\n");
        if (parent->agent_id > 0) agent_proc_exited(parent->agent_id);
        return 0;
    }

    process_t* p = &table[slot];
    p->in_interrupt = 0;
    p->as = vmm_create_address_space();
    if (!p->as.pml4) {
        if (parent->agent_id > 0) agent_proc_exited(parent->agent_id);
        return 0;
    }

    if (vmm_clone_user_pages(&p->as, &parent->as, parent->vma_list) != 0) {
        terminal_writestring("[proc] fork: failed to clone address space (out of memory?)\n");
        vmm_destroy_address_space(&p->as);
        if (parent->agent_id > 0) agent_proc_exited(parent->agent_id);
        return 0;
    }

    thread_group_t* tg = thread_group_alloc();
    if (!tg) {
        vmm_destroy_address_space(&p->as);
        if (parent->agent_id > 0) agent_proc_exited(parent->agent_id);
        return 0;
    }

    p->vma_list = vma_clone_list(parent->vma_list);
    p->aslr_mmap_offset  = parent->aslr_mmap_offset;
    p->aslr_stack_offset = parent->aslr_stack_offset;
    p->aslr_brk_offset   = parent->aslr_brk_offset;
    p->aslr_pie_offset   = parent->aslr_pie_offset;
    p->fs_base           = parent->fs_base;
    p->gs_base           = parent->gs_base;
    p->clear_child_tid   = parent->clear_child_tid;
    p->robust_list_head  = 0;
    memset(&p->sigaltstack, 0, sizeof(p->sigaltstack));

    p->kstack_slot = slot;
    if (kstack_alloc(slot, &p->kernel_stack_base, &p->kernel_stack_top) != 0) {
        vmm_destroy_address_space(&p->as);
        thread_group_free(tg);
        if (parent->agent_id > 0) agent_proc_exited(parent->agent_id);
        return 0;
    }

    tg->tgid = next_pid++;
    tg->ref_count = 1;
    tg->num_threads = 1;
    tg->as = p->as;
    tg->vma_list = p->vma_list;
    tg->heap_end = parent->heap_end;
    tg->heap_mapped_up_to = parent->heap_mapped_up_to;
    tg->ppid = parent->pid;
    tg->pgid = parent->pgid;
    tg->sid = parent->sid;
    tg->agent_id = parent->agent_id;

    p->group = tg;
    p->pid = tg->tgid;
    p->tid = tg->tgid;
    p->ppid = parent->pid;
    p->pgid = parent->pgid;
    p->sid = parent->sid;
    p->fds = tg->fds;
    p->fd_flags = tg->fd_flags;
    p->cwd = tg->cwd;
    p->sig_actions = tg->sig_actions;

    p->is_kernel_task = 0;
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
    p->current_op_flags = 0;
    p->agent_id = parent->agent_id;
    p->ctty = parent->ctty;

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
    while (parent->cwd[c] && c < 1023) {
        p->cwd[c] = parent->cwd[c];
        c++;
    }
    p->cwd[c] = '\0';

    int ex = 0;
    while (parent->exe_path[ex] && ex < 255) {
        p->exe_path[ex] = parent->exe_path[ex];
        ex++;
    }
    p->exe_path[ex] = '\0';
    p->umask_val = parent->umask_val;
    for (int u = 0; u < 8; u++) p->logged_unimpl_syscalls[u] = 0;

    int i = 0; for (; parent->name[i] && i < 26; i++) p->name[i] = parent->name[i];
    p->name[i] = 0;
    const char* suffix = "-fork";
    for (int j = 0; suffix[j] && i < 31; j++, i++) p->name[i] = suffix[j];
    p->name[i] = '\0';

    p->is_forked = 1;
    p->saved_regs = *parent_regs;
    p->saved_regs.rax = 0;

    fabricate_initial_frame(p, process_trampoline);
    p->state = PROC_READY;

    return p;
}

int process_clone(process_t* parent, unsigned long clone_flags, void* child_stack, int* ptid, int* ctid, unsigned long newtls, const struct registers* parent_regs, process_t** out_child) {
    if (!parent || !parent_regs || !out_child) return -EINVAL;

    unsigned long unsupported = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUSER |
                                CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWCGROUP | CLONE_PIDFD | CLONE_PTRACE;
    if (clone_flags & unsupported) {
        return -EINVAL;
    }
    if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND)) {
        return -EINVAL;
    }

    if (parent->agent_id > 0) {
        if (clone_flags & CLONE_THREAD) {
            if (!agent_can_clone_thread(parent->agent_id)) {
                return -EAGAIN;
            }
        } else {
            if (!agent_can_fork(parent->agent_id)) {
                return -EAGAIN;
            }
        }
    }

    /* Reap any zombie secondary threads in this group */
    if (parent->group) {
        for (int i = 1; i < MAX_PROCESSES; i++) {
            if (&table[i] != scheduler_current() && table[i].group == parent->group && table[i].tid != table[i].pid && table[i].state == PROC_ZOMBIE) {
                process_destroy(&table[i]);
            }
        }
    }

    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (table[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        return -EAGAIN;
    }

    process_t* p = &table[slot];
    p->in_interrupt = 0;

    if (clone_flags & CLONE_THREAD) {
        p->group = parent->group;
        p->group->ref_count++;
        p->group->num_threads++;
        p->pid = parent->group->tgid;
        p->tid = next_pid++;
        p->ppid = parent->ppid;
        p->pgid = parent->pgid;
        p->sid = parent->sid;
        p->as = parent->group->as;
        p->vma_list = parent->group->vma_list;
        p->heap_end = parent->group->heap_end;
        p->heap_mapped_up_to = parent->group->heap_mapped_up_to;
        p->fds = parent->group->fds;
        p->fd_flags = parent->group->fd_flags;
        p->cwd = parent->group->cwd;
        p->sig_actions = parent->group->sig_actions;
        p->agent_id = parent->agent_id;
        if (p->agent_id > 0) {
            agent_thread_cloned(p->agent_id);
        }
    } else {
        thread_group_t* tg = thread_group_alloc();
        if (!tg) return -ENOMEM;
        p->group = tg;
        tg->tgid = next_pid++;
        tg->ref_count = 1;
        tg->num_threads = 1;
        tg->ppid = (clone_flags & CLONE_PARENT) ? parent->ppid : parent->pid;
        tg->pgid = parent->pgid;
        tg->sid = parent->sid;
        tg->agent_id = parent->agent_id;
        p->pid = tg->tgid;
        p->tid = tg->tgid;
        p->ppid = tg->ppid;
        p->pgid = tg->pgid;
        p->sid = tg->sid;
        p->agent_id = tg->agent_id;

        if (clone_flags & CLONE_VM) {
            p->as = parent->as;
            tg->as = parent->as;
            p->vma_list = parent->vma_list;
            tg->vma_list = parent->vma_list;
        } else {
            p->as = vmm_create_address_space();
            if (!p->as.pml4) {
                thread_group_free(tg);
                return -ENOMEM;
            }
            if (vmm_clone_user_pages(&p->as, &parent->as, parent->vma_list) != 0) {
                vmm_destroy_address_space(&p->as);
                thread_group_free(tg);
                return -ENOMEM;
            }
            p->vma_list = vma_clone_list(parent->vma_list);
            tg->as = p->as;
            tg->vma_list = p->vma_list;
        }

        if (clone_flags & CLONE_FILES) {
            p->fds = parent->fds;
            p->fd_flags = parent->fd_flags;
        } else {
            for (int i = 0; i < MAX_FDS; i++) {
                tg->fds[i] = parent->fds[i];
                tg->fd_flags[i] = parent->fd_flags[i];
                if (tg->fds[i]) open_file_ref(tg->fds[i]);
            }
            p->fds = tg->fds;
            p->fd_flags = tg->fd_flags;
        }

        if (clone_flags & CLONE_FS) {
            p->cwd = parent->cwd;
        } else {
            int c = 0;
            while (parent->cwd[c] && c < 1023) { tg->cwd[c] = parent->cwd[c]; c++; }
            tg->cwd[c] = '\0';
            p->cwd = tg->cwd;
        }

        if (clone_flags & CLONE_SIGHAND) {
            p->sig_actions = parent->sig_actions;
        } else {
            for (int k = 0; k < NSIG; k++) tg->sig_actions[k] = parent->sig_actions[k];
            p->sig_actions = tg->sig_actions;
        }

        tg->heap_end = parent->heap_end;
        tg->heap_mapped_up_to = parent->heap_mapped_up_to;
        p->heap_end = parent->heap_end;
        p->heap_mapped_up_to = parent->heap_mapped_up_to;

        if (p->agent_id > 0) {
            agent_t* ag = agent_get_by_id(p->agent_id);
            if (ag) ag->usage.procs_count++;
        }
    }

    p->aslr_mmap_offset = parent->aslr_mmap_offset;
    p->aslr_stack_offset = parent->aslr_stack_offset;
    p->aslr_brk_offset = parent->aslr_brk_offset;
    p->aslr_pie_offset = parent->aslr_pie_offset;
    p->fs_base = (clone_flags & CLONE_SETTLS) ? newtls : parent->fs_base;
    p->gs_base = parent->gs_base;
    p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? ctid : NULL;
    p->robust_list_head = NULL;
    memset(&p->sigaltstack, 0, sizeof(p->sigaltstack));

    p->kstack_slot = slot;
    if (kstack_alloc(slot, &p->kernel_stack_base, &p->kernel_stack_top) != 0) {
        if (!(clone_flags & CLONE_THREAD)) thread_group_free(p->group);
        else { p->group->ref_count--; p->group->num_threads--; }
        p->state = PROC_UNUSED;
        return -ENOMEM;
    }

    p->is_kernel_task = 0;
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
    p->current_op_flags = 0;
    p->ctty = parent->ctty;
    p->umask_val = parent->umask_val;
    for (int u = 0; u < 8; u++) p->logged_unimpl_syscalls[u] = 0;

    int ex = 0;
    while (parent->exe_path[ex] && ex < 255) { p->exe_path[ex] = parent->exe_path[ex]; ex++; }
    p->exe_path[ex] = '\0';

    int i = 0; for (; parent->name[i] && i < 26; i++) p->name[i] = parent->name[i];
    p->name[i] = 0;
    if (clone_flags & CLONE_THREAD) {
        const char* sfx = "-th";
        for (int j = 0; sfx[j] && i < 31; j++, i++) p->name[i] = sfx[j];
    } else {
        const char* sfx = "-fork";
        for (int j = 0; sfx[j] && i < 31; j++, i++) p->name[i] = sfx[j];
    }
    p->name[i] = '\0';

    fpu_save(parent);
    fpu_copy(p, parent);

    p->is_forked = 1;
    p->saved_regs = *parent_regs;
    p->saved_regs.rax = 0;
    if (child_stack) {
        p->saved_regs.rsp = (uint64_t) child_stack;
    }

    if ((clone_flags & CLONE_CHILD_SETTID) && ctid) {
        copy_to_user(ctid, &p->tid, sizeof(int));
    }
    if ((clone_flags & CLONE_PARENT_SETTID) && ptid) {
        copy_to_user(ptid, &p->tid, sizeof(int));
    }

    fabricate_initial_frame(p, process_trampoline);
    p->state = PROC_READY;

    *out_child = p;
    return 0;
}
