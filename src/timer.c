#include "timer.h"
#include "idt.h"
#include "io.h"
#include "scheduler.h"
#include "process.h"
#include "signal.h"

static volatile uint32_t tick_count = 0;
static volatile uint32_t idle_tick_count = 0;
static process_t* sleep_queue_head = 0;

void timer_sleep_enqueue(process_t* p, uint32_t deadline) {
    if (!p) return;
    p->sleep_deadline = deadline;
    p->sleep_next = 0;

    if (!sleep_queue_head || deadline < sleep_queue_head->sleep_deadline) {
        p->sleep_next = sleep_queue_head;
        sleep_queue_head = p;
        return;
    }

    process_t* cur = sleep_queue_head;
    while (cur->sleep_next && cur->sleep_next->sleep_deadline <= deadline) {
        cur = cur->sleep_next;
    }
    p->sleep_next = cur->sleep_next;
    cur->sleep_next = p;
}

void timer_sleep_dequeue(process_t* p) {
    if (!p || !sleep_queue_head) return;

    if (sleep_queue_head == p) {
        sleep_queue_head = p->sleep_next;
        p->sleep_next = 0;
        p->sleep_deadline = 0;
        return;
    }

    process_t* cur = sleep_queue_head;
    while (cur->sleep_next && cur->sleep_next != p) {
        cur = cur->sleep_next;
    }
    if (cur->sleep_next == p) {
        cur->sleep_next = p->sleep_next;
        p->sleep_next = 0;
        p->sleep_deadline = 0;
    }
}

static void timer_callback(struct registers* regs) {
    (void) regs;
    tick_count++;

    process_t* cur = scheduler_current();
    if (cur && !cur->is_kernel_task) {
        cur->cpu_ticks++;
    } else {
        idle_tick_count++;
    }

    /* Process sorted sleep timer queue: wake tasks whose deadline has passed */
    while (sleep_queue_head && tick_count >= sleep_queue_head->sleep_deadline) {
        process_t* p = sleep_queue_head;
        sleep_queue_head = p->sleep_next;
        p->sleep_next = 0;
        p->sleep_deadline = 0;
        if (p->state == PROC_WAITING && !p->is_stopped) {
            p->state = PROC_READY;
            p->wait_channel = 0;
        }
    }

    /* Process alarms */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p && p->state != PROC_UNUSED && p->state != PROC_ZOMBIE && p->alarm_ticks > 0) {
            if (--p->alarm_ticks == 0) {
                sig_send(p, SIGALRM);
            }
        }
    }

    scheduler_tick(); /* no-op until scheduler_start() has been called */
}

uint32_t timer_get_ticks(void) {
    return tick_count;
}

uint32_t timer_get_idle_ticks(void) {
    return idle_tick_count;
}

void timer_install(uint32_t frequency) {
    register_interrupt_handler(32, &timer_callback);

    uint32_t divisor = 1193182 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}
