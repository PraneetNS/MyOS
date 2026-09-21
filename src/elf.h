#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Parses an ELF32 executable already loaded into memory at `image`,
   maps its PT_LOAD segments to their p_vaddr (our paging identity-maps
   the first 16MB, so "map" here just means "copy to that physical
   address"), allocates a user stack, and jumps to it via enter_usermode.
   Returns -1 without launching anything if the image isn't a valid
   ELF32 executable for this architecture. */
int elf_load_and_run(const uint8_t* image, uint32_t image_size);

#endif
