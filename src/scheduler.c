#include "scheduler.h"
#include "tss.h"
#include "vmm.h"
#include "vga.h"

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
        process_t* p = process_table_entry((idx + step) % MAX_PROCESSES);
        if (p->state == PROC_READY) return p;
    }
    return process_get_shell(); /* the shell is always READY; safety net */
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

void scheduler_tick(void) {
    if (!started) return;
    if (++tick_counter < SWITCH_EVERY_N_TICKS) return;
    tick_counter = 0;

    process_t* next = pick_next_from(process_index(current));
    if (next == current) return;

    process_t* prev = current;
    current = next;
    enter_process(next);
    switch_task(&prev->esp, next->esp);
}

void scheduler_start(void) {
    started = 1;
    process_t* shell = process_get_shell();
    current = shell;
    enter_process(shell);
    switch_task(&discard_esp, shell->esp); /* never returns */

    for (;;) asm volatile ("hlt"); /* unreachable */
}

void scheduler_exit_current(void) {
    process_t* p = current;
    int idx = process_index(p);
    int exiting_pid = p->pid;
    p->state = PROC_EXITED;

    wake_waiters_for(exiting_pid); /* let any parent blocked in sys_spawn_wait proceed */

    process_t* next = pick_next_from(idx); /* find a successor while p's slot is still identifiable */
    process_destroy(p);                     /* reclaim its frames and kernel stack */

    current = next;
    enter_process(next);
    switch_task(&discard_esp, next->esp); /* p is gone -- nothing to save, never returns here */

    for (;;) asm volatile ("hlt"); /* unreachable */
}

void scheduler_wait_for(int child_pid) {
    process_t* me = current;
    me->state = PROC_WAITING;
    me->waiting_for_pid = child_pid;

    process_t* next = pick_next_from(process_index(me));
    current = next;
    enter_process(next);
    switch_task(&me->esp, next->esp); /* returns here once woken (state back to READY) and rescheduled */
}
