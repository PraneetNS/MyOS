#ifndef FPU_H
#define FPU_H

#include <stdint.h>
#include <stddef.h>
#include "process.h"

void fpu_init(void);
int fpu_has_fpu(void);
int fpu_has_fxsr(void);
int fpu_has_sse(void);
int fpu_has_sse2(void);

void fpu_init_proc(process_t* p);
void fpu_copy(process_t* dst, process_t* src);
void fpu_save(process_t* p);
void fpu_restore(process_t* p);
void fpu_switch(process_t* prev, process_t* next);

void fpu_save_to(uint8_t* dst_512);
void fpu_restore_from(const uint8_t* src_512);
void fpu_proc_set_state(process_t* p, const uint8_t* state_512);
uint8_t* fpu_get_state_ptr(process_t* p);

#endif
