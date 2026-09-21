#include "elf.h"
#include "vga.h"
#include "kheap.h"
#include "usermode.h"

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

#define PT_LOAD 1
#define EM_386  3
#define ET_EXEC 2

#define USER_ELF_STACK_SIZE 8192

int elf_load_and_run(const uint8_t* image, uint32_t image_size) {
    if (image_size < sizeof(Elf32_Ehdr)) {
        terminal_writestring("[elf] file too small to be an ELF\n");
        return -1;
    }

    const Elf32_Ehdr* eh = (const Elf32_Ehdr*) image;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        terminal_writestring("[elf] not an ELF file (bad magic)\n");
        return -1;
    }
    if (eh->e_ident[4] != 1 /* ELFCLASS32 */) {
        terminal_writestring("[elf] not a 32-bit ELF\n");
        return -1;
    }
    if (eh->e_machine != EM_386) {
        terminal_writestring("[elf] not an x86 (EM_386) binary\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        terminal_writestring("[elf] not an executable ELF (expected ET_EXEC)\n");
        return -1;
    }

    terminal_writestring("[elf] valid ELF32/x86 executable, entry=0x");
    { /* tiny inline hex printer */
        uint32_t v = eh->e_entry;
        char hex[9]; hex[8] = '\0';
        const char* digits = "0123456789ABCDEF";
        for (int i = 7; i >= 0; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
        terminal_writestring(hex);
    }
    terminal_writestring("\n");

    const Elf32_Phdr* phdrs = (const Elf32_Phdr*)(image + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        /* NOTE: this "maps" a segment by copying to its physical address
           directly, relying on paging.c's current identity map -- see
           Stage 4/5 README notes on per-process address spaces being
           future work. A real loader would allocate fresh physical
           frames and map them at p_vaddr in a *new* page directory. */
        uint8_t* dest = (uint8_t*) ph->p_vaddr;
        const uint8_t* src = image + ph->p_offset;

        for (uint32_t b = 0; b < ph->p_filesz; b++) dest[b] = src[b];
        for (uint32_t b = ph->p_filesz; b < ph->p_memsz; b++) dest[b] = 0; /* .bss */
    }

    uint8_t* user_stack = (uint8_t*) kmalloc(USER_ELF_STACK_SIZE);
    uint32_t user_stack_top = (uint32_t)(user_stack + USER_ELF_STACK_SIZE);

    terminal_writestring("[elf] segments loaded, entering ring 3...\n");
    enter_usermode(eh->e_entry, user_stack_top); /* never returns */

    return 0; /* unreachable */
}
