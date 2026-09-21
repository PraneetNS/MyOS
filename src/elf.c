#include "elf.h"
#include "vga.h"
#include "kheap.h"
#include "usermode.h"
#include "vmm.h"

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

#define PAGE_SIZE 4096
#define USER_STACK_TOP  0xC0000000u  /* classic convention: stack grows down from the 3GB line */
#define USER_STACK_PAGES 4           /* 16KB stack */

static void print_hex(uint32_t v) {
    char hex[9]; hex[8] = '\0';
    const char* digits = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
    terminal_writestring(hex);
}

/* Maps and zeroes/copies one PT_LOAD segment, page by page, into a
   FRESH physical frame per page -- this is what makes two processes
   loaded at the identical virtual address genuinely isolated: they
   never end up pointing at the same physical memory. */
static int load_segment(address_space_t* as, const Elf32_Phdr* ph, const uint8_t* image) {
    uint32_t start_page = ph->p_vaddr & ~(PAGE_SIZE - 1);
    uint32_t end_addr    = ph->p_vaddr + ph->p_memsz;
    uint32_t end_page    = (end_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t page_vaddr = start_page; page_vaddr < end_page; page_vaddr += PAGE_SIZE) {
        uint32_t frame_phys = vmm_map_user_page(as, page_vaddr);
        if (!frame_phys) return -1;

        uint8_t* frame = (uint8_t*) frame_phys; /* writable directly: kernel's own identity map is still active */
        for (int i = 0; i < PAGE_SIZE; i++) frame[i] = 0; /* covers .bss for free */

        /* Copy whatever part of this page overlaps the segment's file
           data (p_offset..p_offset+p_filesz maps to p_vaddr..+p_filesz). */
        uint32_t seg_file_end = ph->p_vaddr + ph->p_filesz;
        for (uint32_t va = page_vaddr; va < page_vaddr + PAGE_SIZE; va++) {
            if (va < ph->p_vaddr || va >= seg_file_end) continue;
            uint32_t file_off = ph->p_offset + (va - ph->p_vaddr);
            frame[va - page_vaddr] = image[file_off];
        }
    }
    return 0;
}

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
    if (eh->e_ident[4] != 1) { terminal_writestring("[elf] not a 32-bit ELF\n"); return -1; }
    if (eh->e_machine != EM_386) { terminal_writestring("[elf] not an x86 (EM_386) binary\n"); return -1; }
    if (eh->e_type != ET_EXEC) { terminal_writestring("[elf] not an executable ELF (expected ET_EXEC)\n"); return -1; }

    terminal_writestring("[elf] valid ELF32/x86 executable, entry=0x");
    print_hex(eh->e_entry);
    terminal_writestring("\n");

    address_space_t as = vmm_create_address_space();
    if (!as.directory) return -1;

    terminal_writestring("[elf] new address space, page directory phys=0x");
    print_hex(as.directory_phys);
    terminal_writestring("\n");

    const Elf32_Phdr* phdrs = (const Elf32_Phdr*)(image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (load_segment(&as, &phdrs[i], image) != 0) {
            terminal_writestring("[elf] failed to load a segment (out of memory?)\n");
            return -1;
        }
    }

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t page_vaddr = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        if (!vmm_map_user_page(&as, page_vaddr)) {
            terminal_writestring("[elf] failed to map user stack\n");
            return -1;
        }
    }

    terminal_writestring("[elf] segments loaded into isolated frames, switching address space...\n");
    vmm_switch(&as);

    enter_usermode(eh->e_entry, USER_STACK_TOP); /* never returns */

    return 0; /* unreachable */
}
