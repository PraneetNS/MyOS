#include "elf.h"
#include "vga.h"
#include "vmm.h"
#include "paging.h"
#include "process.h"

#define EI_NIDENT 16

/* Standard ELF64 Header */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* Standard ELF64 Program Header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD    1
#define EM_X86_64 62
#define ET_EXEC    2

#define PAGE_SIZE 4096ULL

static void print_hex64(uint64_t v) {
    char hex[17]; hex[16] = '\0';
    const char* digits = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
    terminal_writestring(hex);
}

int elf_load_into(const uint8_t* image, uint64_t image_size,
                  struct vnode* vn,
                  address_space_t* as, int argc, const char* const* argv,
                  int envc, const char* const* envp,
                  uint64_t* out_entry, uint64_t* out_stack_top,
                  vma_t** out_vmas) {
    if (out_vmas) *out_vmas = NULL;
    vma_t* vmas = NULL;

    if (image_size < sizeof(Elf64_Ehdr)) {
        terminal_writestring("[elf] file too small to be an ELF64\n");
        return -1;
    }

    const Elf64_Ehdr* eh = (const Elf64_Ehdr*) image;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        terminal_writestring("[elf] not an ELF file (bad magic)\n");
        return -1;
    }

    /* Verify ELF64 (Class 2), Little-Endian (Data 1), Machine x86-64 (EM_X86_64 = 62) */
    if (eh->e_ident[4] != 2) {
        terminal_writestring("[elf] ELF32 binaries are retired; expected ELF64\n");
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        terminal_writestring("[elf] not an x86-64 (EM_X86_64) binary\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        terminal_writestring("[elf] not an executable ELF (expected ET_EXEC)\n");
        return -1;
    }

    terminal_writestring("[elf] valid ELF64/x86-64 executable, entry=0x");
    print_hex64(eh->e_entry);
    terminal_writestring("\n");

    const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_start = phdrs[i].p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = (phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
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
    uint64_t stack_start = USER_STACK_TOP - PAGE_SIZE;
    uint64_t stack_end   = USER_STACK_TOP;
    vma_t* stack_vma = vma_create(stack_start, stack_end, VMA_PROT_READ | VMA_PROT_WRITE,
                                  VMA_FLAG_ANON | VMA_FLAG_STACK, NULL, 0, 0);
    if (stack_vma) {
        vma_insert(&vmas, stack_vma);
    }

    /* Map initial top stack page */
    uint64_t top_page_vaddr = USER_STACK_TOP - PAGE_SIZE;
    uint64_t top_frame_phys = vmm_map_user_page(as, top_page_vaddr);
    if (!top_frame_phys) {
        terminal_writestring("[elf] failed to map user stack\n");
        vma_free_list(vmas);
        return -1;
    }

    if (argc < 0 || !argv) argc = 0;
    if (argc > 32) argc = 32;
    if (envc < 0 || !envp) envc = 0;
    if (envc > 32) envc = 32;

    uint64_t str_vaddrs[32];
    uint64_t env_vaddrs[32];
    uint64_t offset = 0;
    uint8_t* page_end = (uint8_t*) P2V(top_frame_phys) + PAGE_SIZE;

    /* Copy envp strings down from top of stack page */
    for (int i = envc - 1; i >= 0; i--) {
        const char* s = envp[i] ? envp[i] : "";
        uint64_t len = 0;
        while (s[len]) len++;
        len++;

        offset += len;
        for (uint64_t j = 0; j < len; j++) {
            *(page_end - offset + j) = s[j];
        }
        env_vaddrs[i] = USER_STACK_TOP - offset;
    }

    /* Copy argv strings down from top of stack page */
    for (int i = argc - 1; i >= 0; i--) {
        const char* s = argv[i] ? argv[i] : "";
        uint64_t len = 0;
        while (s[len]) len++;
        len++;

        offset += len;
        for (uint64_t j = 0; j < len; j++) {
            *(page_end - offset + j) = s[j];
        }
        str_vaddrs[i] = USER_STACK_TOP - offset;
    }

    /* 8-byte align offset */
    offset = (offset + 7) & ~7ULL;

    /* NULL envp terminator */
    offset += 8;
    *(uint64_t*)(page_end - offset) = 0;

    /* envp[i] pointers */
    for (int i = envc - 1; i >= 0; i--) {
        offset += 8;
        *(uint64_t*)(page_end - offset) = env_vaddrs[i];
    }

    /* NULL argv[argc] terminator */
    offset += 8;
    *(uint64_t*)(page_end - offset) = 0;

    /* argv[i] pointers */
    for (int i = argc - 1; i >= 0; i--) {
        offset += 8;
        *(uint64_t*)(page_end - offset) = str_vaddrs[i];
    }

    /* argc */
    offset += 8;
    *(uint64_t*)(page_end - offset) = (uint64_t) argc;

    *out_entry = eh->e_entry;
    *out_stack_top = USER_STACK_TOP - offset;
    if (out_vmas) *out_vmas = vmas;
    return 0;
}
