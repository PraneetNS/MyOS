#include "scheduler.h"
#include "tss.h"
#include "vmm.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "bcache.h"
#include "tty.h"
#include "fpu.h"
#include "console.h"
#include "signal.h"
#include "agent.h"
#include "futex.h"
#include "uaccess.h"
#include "vma.h"

#define SWITCH_EVERY_N_TICKS 30 /* at 100Hz, ~0.3s per process's time slice */

extern void switch_task(uint64_t* old_rsp_store, uint64_t new_rsp);

static process_t* current = 0;
static uint32_t tick_counter = 0;
static int started = 0;
static uint64_t discard_rsp; /* landing spot for switch_task's "old rsp" when there's no real predecessor to save */

process_t* scheduler_current(void) { return current; }

static int process_index(process_t* p) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table_entry(i) == p) return i;
    return 0;
}

static process_t* pick_next_from(int idx) {
    for (int step = 1; step <= MAX_PROCESSES; step++) {
        int candidate = (idx + step) % MAX_PROCESSES;
        if (candidate == 0) continue;
        process_t* p = process_table_entry(candidate);
        if (p && p->state == PROC_READY && !p->is_stopped) {
            if (p->agent_id > 0 && !agent_is_runnable(p->agent_id)) {
                if (!(p->sig_pending & (sigmask(SIGKILL) | sigmask(SIGTERM)))) {
                    continue;
                }
            }
            return p;
        }
    }
    process_t* p0 = process_table_entry(0);
    if (p0 && p0->state == PROC_READY) return p0;
    return 0;
}

static void enter_process(process_t* next) {
    tss_set_kernel_stack(next->kernel_stack_top);
    vmm_switch(&next->as);
    if (!next->is_kernel_task) {
        wrmsr64(MSR_FS_BASE, next->fs_base);
    }
}

void wake_waiters_for(int pid) {
    process_t* exiting = process_find_by_pid(pid);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (!p || p->state != PROC_WAITING || p->is_stopped || p->pid == pid) continue;
        if (p->wait_channel != 0) continue;

        int wake = 0;
        if (p->waiting_for_pid == pid) {
            wake = 1;
        } else if (p->waiting_for_pid == -1 && exiting && exiting->ppid == p->pid) {
            wake = 1;
        } else if (p->waiting_for_pid == 0 && exiting && exiting->ppid == p->pid && exiting->pgid == p->pgid) {
            wake = 1;
        } else if (p->waiting_for_pid < -1 && exiting && exiting->ppid == p->pid && exiting->pgid == -p->waiting_for_pid) {
            wake = 1;
        }
        if (wake) {
            p->state = PROC_READY;
            p->waiting_for_pid = -999;
        }
    }
}

static int time_slice_for(process_t* p) {
    if (!p) return 10;
    int n = p->nice;
    if (n < -20) n = -20;
    if (n > 19) n = 19;
    int slice = 10 - (n / 2);
    if (slice < 2) slice = 2;
    return slice;
}

static void reap_zombie_threads(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p && p != current && p->state == PROC_ZOMBIE && p->group && p->tid != p->pid) {
            process_destroy(p);
        }
    }
}

void scheduler_yield(void) {
    if (!started || !current) return;
    reap_zombie_threads();
    agent_killswitch_check_deferred();
    process_t* next = pick_next_from(process_index(current));
    if (!next || next == current) return;

    tick_counter = 0;
    process_t* prev = current;
    current = next;
    enter_process(next);
    fpu_switch(prev, next);
    switch_task(&prev->rsp, next->rsp);
}

void scheduler_tick(void) {
    if (!started) return;
    reap_zombie_threads();

    agent_killswitch_check_deferred();

    while (serial_received()) {
        char c = serial_read();
        tty_input_char(global_tty, c);
    }

    if (current && current->is_kernel_task) {
        process_t* next = pick_next_from(0);
        if (next && next != current) {
            tick_counter = 0;
            process_t* prev = current;
            current = next;
            enter_process(next);
            fpu_switch(prev, next);
            switch_task(&prev->rsp, next->rsp);
            return;
        }
    }

    if (current && current->agent_id > 0 && !agent_is_runnable(current->agent_id)) {
        if (!(current->sig_pending & (sigmask(SIGKILL) | sigmask(SIGTERM)))) {
            scheduler_yield();
            return;
        }
    }

    int slice = time_slice_for(current);
    if (++tick_counter < (uint32_t)slice) return;
    tick_counter = 0;

    process_t* next = pick_next_from(process_index(current));
    if (!next || next == current) return;

    process_t* prev = current;
    current = next;
    enter_process(next);
    fpu_switch(prev, next);
    switch_task(&prev->rsp, next->rsp);
}

void scheduler_start(void) {
    started = 1;
    process_t* first = pick_next_from(0);
    if (!first) first = process_get_shell();
    current = first;
    enter_process(first);
    fpu_switch(NULL, first);
    switch_task(&discard_rsp, first->rsp); /* never returns */

    for (;;) asm volatile ("hlt"); /* unreachable */
}

void scheduler_exit_current(int exit_status) {
    process_t* p = current;
    int idx = process_index(p);
    int exiting_pid = p->pid;

    /* Handle futex robust list and clear_child_tid before unmapping address space */
    futex_handle_thread_exit(p);
    if (p->clear_child_tid) {
        int zero = 0;
        copy_to_user(p->clear_child_tid, &zero, sizeof(int));
        futex_wake((uint32_t*)p->clear_child_tid, 1, 0xFFFFFFFF);
        p->clear_child_tid = 0;
    }

    int is_secondary = (p->group && p->group->num_threads > 1 && p->tid != p->pid);
    if (is_secondary) {
        p->group->num_threads--;
        if (p->agent_id > 0) {
            agent_thread_exited(p->agent_id);
            p->agent_id = 0;
        }
        p->exit_code = exit_status;
        p->state = PROC_ZOMBIE;
        p->as.pml4 = 0;

        vmm_switch_to_kernel();
        wake_waiters_for(p->tid);
    } else {
        if (exiting_pid == 1) {
            bcache_sync();
        }

        console_on_process_exit(exiting_pid);

        /* Reparent children to PID 1 (init / shell) */
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t* child = process_table_entry(i);
            if (child && child->state != PROC_UNUSED && child->ppid == exiting_pid) {
                child->ppid = 1;
            }
        }

        /* If there are any other threads in this group, mark them zombie */
        if (p->group) {
            for (int i = 0; i < MAX_PROCESSES; i++) {
                process_t* other = process_table_entry(i);
                if (other && other != p && other->group == p->group && other->state != PROC_UNUSED) {
                    other->state = PROC_ZOMBIE;
                    if (other->agent_id > 0) {
                        agent_thread_exited(other->agent_id);
                        other->agent_id = 0;
                    }
                }
            }
            p->group->num_threads = 0;

            /* Close all open descriptors */
            for (int i = 0; i < MAX_FDS; i++) {
                p->group->fd_flags[i] = 0;
                if (p->group->fds[i]) {
                    open_file_unref(p->group->fds[i]);
                    p->group->fds[i] = 0;
                }
            }
        } else {
            for (int i = 0; i < MAX_FDS; i++) {
                p->fd_flags[i] = 0;
                if (p->fds[i]) {
                    open_file_unref(p->fds[i]);
                    p->fds[i] = 0;
                }
            }
        }

        vmm_switch_to_kernel();

        /* Release user address space */
        if (p->group && !p->is_kernel_task && p->group->as.pml4 && p->group->tgid != 0) {
            vmm_destroy_address_space(&p->group->as);
            p->group->as.pml4 = 0;
            p->as.pml4 = 0;
        } else if (!p->group && !p->is_kernel_task && p->as.pml4) {
            vmm_destroy_address_space(&p->as);
            p->as.pml4 = 0;
        }

        if (p->group && p->group->vma_list) {
            vma_free_list(p->group->vma_list);
            p->group->vma_list = NULL;
            p->vma_list = NULL;
        } else if (!p->group && p->vma_list) {
            vma_free_list(p->vma_list);
            p->vma_list = NULL;
        }

        if (p->agent_id > 0) {
            agent_proc_exited(p->agent_id);
            p->agent_id = 0;
        }

        p->exit_code = exit_status;
        p->state = PROC_ZOMBIE;

        /* Restore TTY foreground group to parent if foreground group has no living processes */
        extern tty_t* global_tty;
        if (global_tty && global_tty->fg_pgid == p->pgid) {
            int any_alive = 0;
            for (int i = 0; i < MAX_PROCESSES; i++) {
                process_t* other = process_table_entry(i);
                if (other && other != p && other->state != PROC_UNUSED && other->state != PROC_ZOMBIE && other->pgid == p->pgid) {
                    any_alive = 1;
                    break;
                }
            }
            if (!any_alive) {
                process_t* parent_p = process_find_by_pid(p->ppid);
                if (parent_p) {
                    global_tty->fg_pgid = parent_p->pgid;
                } else {
                    global_tty->fg_pgid = 1;
                }
            }
        }

        process_t* parent = process_find_by_pid(p->ppid);
        if (parent) {
            sig_send(parent, SIGCHLD);
        }

        /* Adopt orphaned children into PID 1 */
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t* child = process_table_entry(i);
            if (child && child != p && child->state != PROC_UNUSED && child->ppid == exiting_pid) {
                child->ppid = 1;
                if (child->state == PROC_ZOMBIE) {
                    process_t* init_p = process_find_by_pid(1);
                    if (init_p && init_p != p) {
                        sig_send(init_p, SIGCHLD);
                    }
                }
            }
        }

        wake_waiters_for(exiting_pid);
    }

    extern int g_kernel_shutting_down;
    if (exiting_pid == 1 && !g_kernel_shutting_down) {
        kprintf("\n[PANIC] Attempted to kill init! System halted.\n");
        bcache_sync();
        for (;;) { asm volatile ("cli; hlt"); }
    }

    process_t* next = pick_next_from(idx);
    while (!next) {
        while (serial_received()) {
            char c = serial_read();
            tty_input_char(global_tty, c);
        }
        asm volatile ("sti; hlt");
        next = pick_next_from(idx);
    }

    current = next;
    enter_process(next);
    fpu_switch(NULL, next);
    switch_task(&discard_rsp, next->rsp); /* p is gone -- nothing to save, never returns here */

    for (;;) asm volatile ("hlt"); /* unreachable */
}

void scheduler_wait_or_idle(process_t* me) {
    process_t* next = pick_next_from(process_index(me));
    if (next) {
        process_t* prev = me;
        current = next;
        enter_process(next);
        fpu_switch(prev, next);
        switch_task(&me->rsp, next->rsp);
    } else {
        while (me->state == PROC_WAITING) {
            while (serial_received()) {
                char c = serial_read();
                tty_input_char(global_tty, c);
            }
            if (me->state != PROC_WAITING) break;
            asm volatile ("sti; hlt");
        }
    }
}

void scheduler_wait_for(int child_pid) {
    process_t* me = current;
    me->state = PROC_WAITING;
    me->waiting_for_pid = child_pid;
    scheduler_wait_or_idle(me);
    me->waiting_for_pid = -999;
}

void scheduler_wait_channel(void* channel) {
    process_t* me = current;
    me->state = PROC_WAITING;
    me->wait_channel = channel;
    scheduler_wait_or_idle(me);
    me->wait_channel = 0;
    me->num_poll_channels = 0;
}

void scheduler_wait_poll(void) {
    process_t* me = current;
    if (!me) return;
    me->state = PROC_WAITING;
    me->wait_channel = 0;
    scheduler_wait_or_idle(me);
    me->wait_channel = 0;
    me->num_poll_channels = 0;
}

void scheduler_wake_channel(void* channel) {
    if (!channel) return;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (!p || p->state != PROC_WAITING) continue;
        if (p->wait_channel == channel) {
            p->state = PROC_READY;
            p->wait_channel = 0;
            p->num_poll_channels = 0;
            continue;
        }
        for (int c = 0; c < p->num_poll_channels; c++) {
            if (p->poll_channels[c] == channel) {
                p->state = PROC_READY;
                p->wait_channel = 0;
                p->num_poll_channels = 0;
                break;
            }
        }
    }
}

