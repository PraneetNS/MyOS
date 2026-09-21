#include "task.h"
#include "kheap.h"
#include "vga.h"

#define MAX_TASKS      4
#define TASK_STACK_SIZE 8192
#define SWITCH_EVERY_N_TICKS 30   /* at 100Hz, switch every ~0.3s */

typedef struct {
    uint32_t esp;
    int in_use;
} task_t;

static task_t tasks[MAX_TASKS];
static int task_count = 0;
static int current_task = -1;
static int scheduler_running = 0;
static uint32_t tick_counter = 0;
static uint32_t discard_esp; /* landing spot for the very first switch's "old esp" */

extern void switch_task(uint32_t* old_esp_store, uint32_t new_esp);

void task_create(void (*entry)(void)) {
    if (task_count >= MAX_TASKS) return;

    uint8_t* stack_base = (uint8_t*) kmalloc(TASK_STACK_SIZE);
    uint32_t* sp = (uint32_t*)(stack_base + TASK_STACK_SIZE);

    /* Build a fake switch_task frame: when switch_task "resumes" this
       task for the first time, it pops these values off in this order
       (popf, edi, esi, ebx, ebp) and then `ret`s into entry(). */
    *(--sp) = (uint32_t) entry;
    *(--sp) = 0; /* ebp */
    *(--sp) = 0; /* ebx */
    *(--sp) = 0; /* esi */
    *(--sp) = 0; /* edi */
    *(--sp) = 0x202; /* eflags, IF set */

    tasks[task_count].esp = (uint32_t) sp;
    tasks[task_count].in_use = 1;
    task_count++;
}

void scheduler_tick(void) {
    if (!scheduler_running) return;
    if (++tick_counter < SWITCH_EVERY_N_TICKS) return;
    tick_counter = 0;

    if (task_count < 2) return; /* nothing to switch to */

    int prev = current_task;
    current_task = (current_task + 1) % task_count;
    switch_task(&tasks[prev].esp, tasks[current_task].esp);
}

void scheduler_start(void) {
    if (task_count == 0) {
        terminal_writestring("[warn] scheduler_start() called with no tasks\n");
        for (;;) asm volatile ("hlt");
    }

    scheduler_running = 1;
    current_task = 0;
    /* This call never returns to its caller: it jumps into task 0's
       entry function and control only ever comes back here via the
       timer IRQ's calls to scheduler_tick() above. */
    switch_task(&discard_esp, tasks[0].esp);

    /* unreachable */
    for (;;) asm volatile ("hlt");
}
