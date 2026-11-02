#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include "vmm.h"
#include "vma.h"

struct vnode;
struct process;

/* Loads an ELF64 x86-64 executable binary into the target address space.
   Supports static non-PIE (ET_EXEC) and static-PIE (ET_DYN).
   Sets up Linux-standard AUXV vector and 16-byte aligned user stack. */
int elf_load_into(const uint8_t* image, uint64_t image_size,
                  struct vnode* vn, struct process* proc,
                  address_space_t* as, int argc, const char* const* argv,
                  int envc, const char* const* envp,
                  uint64_t* out_entry, uint64_t* out_stack_top,
                  vma_t** out_vmas);

#endif
