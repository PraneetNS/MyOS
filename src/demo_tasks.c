#include "demo_tasks.h"
#include "vga.h"
#include <stdint.h>

static void delay(void) {
    for (volatile uint32_t i = 0; i < 2000000; i++) { }
}

static void print_uint(uint32_t n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { terminal_writestring("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(&buf[i]);
}

/* Each task's print is 3 separate terminal_writestring calls (prefix,
   number, newline). Each call is individually protected against
   corruption (see vga.c), but the task could still be preempted
   *between* those calls, splitting one logical line across a
   scheduler switch. Wrapping the whole group in cli/sti makes the
   entire line print atomically. */
void task_a_entry(void) {
    uint32_t counter = 0;
    for (;;) {
        asm volatile ("cli");
        terminal_writestring("[Task A] running, iteration ");
        print_uint(counter++);
        terminal_writestring("\n");
        asm volatile ("sti");
        delay();
    }
}

void task_b_entry(void) {
    uint32_t counter = 0;
    for (;;) {
        asm volatile ("cli");
        terminal_writestring("[Task B] running, iteration ");
        print_uint(counter++);
        terminal_writestring("\n");
        asm volatile ("sti");
        delay();
    }
}
