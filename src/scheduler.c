#include "scheduler.h"
#include "tss.h"
#include "vmm.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "bcache.h"

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
        if (p->state == PROC_WAITING && p->waiting_for_pid == pid) {
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
        keyboard_handle_char(c);
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

void scheduler_exit_current(void) {
    process_t* p = current;
    int idx = process_index(p);
    int exiting_pid = p->pid;
    p->state = PROC_EXITED;

    if (exiting_pid == 1) {
        bcache_sync();
    }

    wake_waiters_for(exiting_pid); /* let any parent blocked in sys_spawn_wait proceed */

    vmm_switch_to_kernel();
    process_destroy(p); /* reclaim its frames and kernel stack */

    if (exiting_pid == 1) {
        kprintf("[init] shell exited, respawning sh.elf...\n");
        process_t* sh = process_spawn_by_name("sh.elf", 0);
        if (sh) {
            kprintf("[init] respawned sh.elf (PID %d)\n", sh->pid);
        }
    }

    process_t* next = pick_next_from(idx);
    while (!next) {
        while (serial_received()) {
            char c = serial_read();
            keyboard_handle_char(c);
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
                keyboard_handle_char(c);
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

#define PIPE_WAIT_SENTINEL (-2) /* distinct from any real pid (>=0) and from -1 ("not waiting") */

void scheduler_wait_for_pipe(void) {
    process_t* me = current;
    me->state = PROC_WAITING;
    me->waiting_for_pid = PIPE_WAIT_SENTINEL;
    wait_or_idle(me);
}

void scheduler_wake_pipe_waiters(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p->state == PROC_WAITING && p->waiting_for_pid == PIPE_WAIT_SENTINEL) {
            p->state = PROC_READY;
            p->waiting_for_pid = -1;
        }
    }
}

#define STDIN_WAIT_SENTINEL (-3)

void scheduler_wait_for_stdin(void) {
    process_t* me = current;
    me->state = PROC_WAITING;
    me->waiting_for_pid = STDIN_WAIT_SENTINEL;
    wait_or_idle(me);
}

void scheduler_wake_stdin_waiters(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p->state == PROC_WAITING && p->waiting_for_pid == STDIN_WAIT_SENTINEL) {
            p->state = PROC_READY;
            p->waiting_for_pid = -1;
        }
    }
    if (started && current && current->is_kernel_task) {
        scheduler_yield();
    }
}
