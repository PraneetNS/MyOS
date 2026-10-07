#include "scheduler.h"
#include "tss.h"
#include "vmm.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "bcache.h"
#include "tty.h"

#define SWITCH_EVERY_N_TICKS 30 /* at 100Hz, ~0.3s per process's time slice */

extern void switch_task(uint32_t* old_esp_store, uint32_t new_esp);

static process_t* current = 0;
static uint32_t tick_counter = 0;
static int started = 0;
static uint32_t discard_esp; /* landing spot for switch_task's "old esp" when there's no real predecessor to save */

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
        if (p && p->state == PROC_READY) return p;
    }
    process_t* p0 = process_table_entry(0);
    if (p0 && p0->state == PROC_READY) return p0;
    return 0;
}

static void enter_process(process_t* next) {
    tss_set_kernel_stack(next->kernel_stack_top);
    vmm_switch(&next->as);
}

static void wake_waiters_for(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p && p->state == PROC_WAITING && (p->waiting_for_pid == pid || p->waiting_for_pid == -1)) {
            p->state = PROC_READY;
            p->waiting_for_pid = -1;
        }
    }
}

void scheduler_yield(void) {
    if (!started || !current) return;
    process_t* next = pick_next_from(process_index(current));
    if (!next || next == current) return;

    process_t* prev = current;
    current = next;
    enter_process(next);
    switch_task(&prev->esp, next->esp);
}

void scheduler_tick(void) {
    if (!started) return;

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
            switch_task(&prev->esp, next->esp);
            return;
        }
    }

    if (++tick_counter < SWITCH_EVERY_N_TICKS) return;
    tick_counter = 0;

    process_t* next = pick_next_from(process_index(current));
    if (!next || next == current) return;

    process_t* prev = current;
    current = next;
    enter_process(next);
    switch_task(&prev->esp, next->esp);
}

void scheduler_start(void) {
    started = 1;
    process_t* first = pick_next_from(0);
    if (!first) first = process_get_shell();
    current = first;
    enter_process(first);
    switch_task(&discard_esp, first->esp); /* never returns */

    for (;;) asm volatile ("hlt"); /* unreachable */
}

void scheduler_exit_current(int exit_status) {
    process_t* p = current;
    int idx = process_index(p);
    int exiting_pid = p->pid;

    if (exiting_pid == 1) {
        bcache_sync();
    }

    /* Reparent children to PID 1 (init / shell) */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* child = process_table_entry(i);
        if (child && child->state != PROC_UNUSED && child->ppid == exiting_pid) {
            child->ppid = 1;
        }
    }

    /* Close all open descriptors */
    for (int i = 0; i < MAX_FDS; i++) {
        p->fd_flags[i] = 0;
        if (p->fds[i]) {
            open_file_unref(p->fds[i]);
            p->fds[i] = 0;
        }
    }

    /* Release user address space */
    if (!p->is_kernel_task && p->as.directory) {
        vmm_destroy_address_space(&p->as);
        p->as.directory = 0;
    }

    p->exit_code = exit_status;
    p->state = PROC_ZOMBIE;

    process_t* parent = process_find_by_pid(p->ppid);
    if (parent) {
        sig_send(parent, SIGCHLD);
    }

    wake_waiters_for(exiting_pid);

    vmm_switch_to_kernel();

    extern int respawn_shell_needed;
    if (exiting_pid == 1) {
        respawn_shell_needed = 1;
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
    switch_task(&discard_esp, next->esp); /* p is gone -- nothing to save, never returns here */

    for (;;) asm volatile ("hlt"); /* unreachable */
}

static void wait_or_idle(process_t* me) {
    process_t* next = pick_next_from(process_index(me));
    if (next) {
        current = next;
        enter_process(next);
        switch_task(&me->esp, next->esp);
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
    wait_or_idle(me);
}

void scheduler_wait_channel(void* channel) {
    process_t* me = current;
    me->state = PROC_WAITING;
    me->wait_channel = channel;
    wait_or_idle(me);
    me->wait_channel = 0;
}

void scheduler_wake_channel(void* channel) {
    if (!channel) return;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p && p->state == PROC_WAITING && p->wait_channel == channel) {
            p->state = PROC_READY;
            p->wait_channel = 0;
        }
    }
}

