#include "elf.h"
#include "vga.h"
#include "vmm.h"
#include "paging.h"
#include "process.h"

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



int elf_load_into(const uint8_t* image, uint32_t image_size,
                  struct vnode* vn,
                  address_space_t* as, int argc, const char* const* argv,
                  uint32_t* out_entry, uint32_t* out_stack_top,
                  vma_t** out_vmas) {
    if (out_vmas) *out_vmas = NULL;
    vma_t* vmas = NULL;

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

    const Elf32_Phdr* phdrs = (const Elf32_Phdr*)(image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint32_t seg_start = phdrs[i].p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t seg_end   = (phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t prot = 0;
        if (phdrs[i].p_flags & 4) prot |= VMA_PROT_READ;
        if (phdrs[i].p_flags & 2) prot |= VMA_PROT_WRITE;
        if (phdrs[i].p_flags & 1) prot |= VMA_PROT_EXEC;
        vma_t* vma = vma_create(seg_start, seg_end, prot, VMA_FLAG_FILE, vn, phdrs[i].p_offset, phdrs[i].p_filesz);
        if (vma) {
            vma_insert(&vmas, vma);
        }
    }

    /* Create heap VMA (starts at HEAP_BASE, initially 0 bytes) */
    vma_t* heap_vma = vma_create(HEAP_BASE, HEAP_BASE, VMA_PROT_READ | VMA_PROT_WRITE,
                                 VMA_FLAG_ANON | VMA_FLAG_HEAP, NULL, 0, 0);
    if (heap_vma) {
        vma_insert(&vmas, heap_vma);
    }

    /* Create stack VMA: starts with initial top page, auto-grows downward on fault */
    uint32_t stack_start = USER_STACK_TOP - PAGE_SIZE;
    uint32_t stack_end   = USER_STACK_TOP;
    vma_t* stack_vma = vma_create(stack_start, stack_end, VMA_PROT_READ | VMA_PROT_WRITE,
                                  VMA_FLAG_ANON | VMA_FLAG_STACK, NULL, 0, 0);
    if (stack_vma) {
        vma_insert(&vmas, stack_vma);
    }

    /* Map only the single top stack page for argv/argc setup */
    uint32_t top_page_vaddr = USER_STACK_TOP - PAGE_SIZE;
    uint32_t top_frame_phys = vmm_map_user_page(as, top_page_vaddr);
    if (!top_frame_phys) {
        terminal_writestring("[elf] failed to map user stack\n");
        vma_free_list(vmas);
        return -1;
    }

    if (argc < 0 || !argv) argc = 0;
    if (argc > 32) argc = 32;

    uint32_t str_vaddrs[32];
    uint32_t offset = 0;
    uint8_t* page_end = (uint8_t*) P2V(top_frame_phys) + PAGE_SIZE;

    /* Copy strings down from the top of the stack page */
    for (int i = argc - 1; i >= 0; i--) {
        const char* s = argv[i] ? argv[i] : "";
        uint32_t len = 0;
        while (s[len]) len++;
        len++; /* include NUL */

        offset += len;
        for (uint32_t j = 0; j < len; j++) {
            *(page_end - offset + j) = s[j];
        }
        str_vaddrs[i] = USER_STACK_TOP - offset;
    }

    /* Word align */
    offset = (offset + 3) & ~3u;

    /* NULL envp */
    offset += 4;
    *(uint32_t*)(page_end - offset) = 0;

    /* NULL argv[argc] terminator */
    offset += 4;
    *(uint32_t*)(page_end - offset) = 0;

    /* argv[i] pointers */
    for (int i = argc - 1; i >= 0; i--) {
        offset += 4;
        *(uint32_t*)(page_end - offset) = str_vaddrs[i];
    }

    /* argc */
    offset += 4;
    *(uint32_t*)(page_end - offset) = (uint32_t) argc;

    *out_entry = eh->e_entry;
    *out_stack_top = USER_STACK_TOP - offset;
    if (out_vmas) *out_vmas = vmas;
    return 0;
}
