#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

struct process;

void timer_install(uint32_t frequency);
uint32_t timer_get_ticks(void);
uint32_t timer_get_idle_ticks(void);

void timer_sleep_enqueue(struct process* p, uint32_t deadline);
void timer_sleep_dequeue(struct process* p);

#endif
