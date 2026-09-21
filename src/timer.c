#include "timer.h"
#include "idt.h"
#include "io.h"
#include "scheduler.h"

static volatile uint32_t tick_count = 0;

static void timer_callback(struct registers* regs) {
    (void) regs;
    tick_count++;
    scheduler_tick(); /* no-op until scheduler_start() has been called */
}

uint32_t timer_get_ticks(void) {
    return tick_count;
}

/* Programs the 8253/8254 PIT (Programmable Interval Timer) to fire
   IRQ0 at the given frequency (Hz). The PIT's oscillator runs at
   ~1.193182 MHz, so we divide that down to hit our target rate. */
void timer_install(uint32_t frequency) {
    register_interrupt_handler(32, &timer_callback); /* IRQ0 = vector 32 */

    uint32_t divisor = 1193182 / frequency;

    outb(0x43, 0x36);                          /* channel 0, mode 3 (square wave) */
    outb(0x40, (uint8_t)(divisor & 0xFF));      /* low byte  */
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF)); /* high byte */
}
