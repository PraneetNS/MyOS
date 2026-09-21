#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include "vmm.h"

/* Parses an ELF32 executable already loaded into memory at `image`,
   maps its PT_LOAD segments (and a user stack) into the GIVEN address
   space using fresh physical frames via vmm.c -- but does NOT enter
   ring 3 itself. The caller (process.c) decides when this process
   actually runs, via the scheduler. Returns 0 on success (with
   *out_entry and *out_stack_top filled in) or -1 on failure. */
int elf_load_into(const uint8_t* image, uint32_t image_size,
                   address_space_t* as, uint32_t* out_entry, uint32_t* out_stack_top);

#endif
