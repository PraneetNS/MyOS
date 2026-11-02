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

#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3
#define PT_NOTE      4
#define PT_SHLIB     5
#define PT_PHDR      6
#define PT_TLS       7
#define PT_GNU_STACK 0x6474e551

#define EM_X86_64   62
#define ET_EXEC      2
#define ET_DYN       3

#define PAGE_SIZE 4096ULL

extern uint32_t random_u32(void);

static void print_hex64(uint64_t v) {
    char hex[17]; hex[16] = '\0';
    const char* digits = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
    terminal_writestring(hex);
}

struct auxv_pair {
    uint64_t type;
    uint64_t val;
};

int elf_load_into(const uint8_t* image, uint64_t image_size,
                  struct vnode* vn, struct process* proc,
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
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        terminal_writestring("[elf] not an executable ELF (expected ET_EXEC or ET_DYN)\n");
        return -1;
    }

    /* Calculate load base for ET_DYN (PIE) vs ET_EXEC (fixed) */
    uint64_t load_base = 0;
    if (eh->e_type == ET_DYN) {
        load_base = 0x0000555555550000ULL + (proc ? proc->aslr_pie_offset : 0);
    }

    terminal_writestring("[elf] valid ELF64/x86-64 binary, type=");
    terminal_writestring(eh->e_type == ET_DYN ? "ET_DYN(PIE)" : "ET_EXEC");
    terminal_writestring(", entry=0x");
    print_hex64(load_base + eh->e_entry);
    terminal_writestring("\n");

    const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(image + eh->e_phoff);
    uint64_t phdr_va = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type == PT_PHDR) {
            phdr_va = load_base + phdrs[i].p_vaddr;
        }

        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t seg_start = (load_base + phdrs[i].p_vaddr) & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = (load_base + phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t prot = 0;
        if (phdrs[i].p_flags & 4) prot |= VMA_PROT_READ;
        if (phdrs[i].p_flags & 2) prot |= VMA_PROT_WRITE;
        if (phdrs[i].p_flags & 1) prot |= VMA_PROT_EXEC;

        vma_t* vma = vma_create(seg_start, seg_end, prot, VMA_FLAG_FILE, vn, phdrs[i].p_offset, phdrs[i].p_filesz);
        if (vma) {
            vma_insert(&vmas, vma);
        }
    }

    if (phdr_va == 0) {
        phdr_va = load_base + eh->e_phoff;
    }

    /* Create heap VMA (starts at HEAP_BASE + aslr_brk_offset, initially 0 bytes) */
    uint64_t heap_start = HEAP_BASE + (proc ? proc->aslr_brk_offset : 0);
    vma_t* heap_vma = vma_create(heap_start, heap_start, VMA_PROT_READ | VMA_PROT_WRITE,
                                 VMA_FLAG_ANON | VMA_FLAG_HEAP, NULL, 0, 0);
    if (heap_vma) {
        vma_insert(&vmas, heap_vma);
    }

    /* Create stack VMA: starts with initial top page, auto-grows downward on fault */
    uint64_t stack_top_max = USER_STACK_TOP - (proc ? proc->aslr_stack_offset : 0);
    stack_top_max &= ~15ULL; /* 16-byte alignment */

    uint64_t top_page_vaddr = (stack_top_max - 1) & ~(PAGE_SIZE - 1);
    vma_t* stack_vma = vma_create(top_page_vaddr, USER_STACK_TOP, VMA_PROT_READ | VMA_PROT_WRITE,
                                  VMA_FLAG_ANON | VMA_FLAG_STACK, NULL, 0, 0);
    if (stack_vma) {
        vma_insert(&vmas, stack_vma);
    }

    /* Map initial top stack page */
    uint64_t top_frame_phys = vmm_map_user_page(as, top_page_vaddr);
    if (!top_frame_phys) {
        terminal_writestring("[elf] failed to map user stack\n");
        vma_free_list(vmas);
        return -1;
    }

    if (argc < 0 || !argv) argc = 0;
    if (argc > 64) argc = 64;
    if (envc < 0 || !envp) envc = 0;
    if (envc > 64) envc = 64;

    uint64_t str_vaddrs[64];
    uint64_t env_vaddrs[64];
    uint8_t* page_ptr = (uint8_t*) P2V(top_frame_phys);
    uint64_t cur_sp = stack_top_max;

    /* 1. 16-byte random bytes for AT_RANDOM */
    cur_sp -= 16;
    uint64_t at_random_va = cur_sp;
    uint32_t* rand_words = (uint32_t*)(page_ptr + (cur_sp - top_page_vaddr));
    for (int k = 0; k < 4; k++) rand_words[k] = random_u32();

    /* 2. Push execfn (argv[0] or binary name) */
    const char* exec_name = (argc > 0 && argv[0]) ? argv[0] : "myos";
    uint64_t exec_len = 0;
    while (exec_name[exec_len]) exec_len++;
    exec_len++;
    cur_sp -= exec_len;
    uint64_t execfn_va = cur_sp;
    for (uint64_t j = 0; j < exec_len; j++) {
        *(page_ptr + (cur_sp - top_page_vaddr + j)) = exec_name[j];
    }

    /* 3. Push envp strings */
    for (int i = envc - 1; i >= 0; i--) {
        const char* s = envp[i] ? envp[i] : "";
        uint64_t len = 0;
        while (s[len]) len++;
        len++;
        cur_sp -= len;
        env_vaddrs[i] = cur_sp;
        for (uint64_t j = 0; j < len; j++) {
            *(page_ptr + (cur_sp - top_page_vaddr + j)) = s[j];
        }
    }

    /* 4. Push argv strings */
    for (int i = argc - 1; i >= 0; i--) {
        const char* s = argv[i] ? argv[i] : "";
        uint64_t len = 0;
        while (s[len]) len++;
        len++;
        cur_sp -= len;
        str_vaddrs[i] = cur_sp;
        for (uint64_t j = 0; j < len; j++) {
            *(page_ptr + (cur_sp - top_page_vaddr + j)) = s[j];
        }
    }

    /* 8-byte align string bottom */
    cur_sp &= ~7ULL;

    /* 5. Prepare AUXV entries */
    struct auxv_pair auxv[16];
    int n_auxv = 0;
    auxv[n_auxv++] = (struct auxv_pair){ 31 /* AT_EXECFN */, execfn_va };
    auxv[n_auxv++] = (struct auxv_pair){ 25 /* AT_RANDOM */, at_random_va };
    auxv[n_auxv++] = (struct auxv_pair){ 23 /* AT_SECURE */, 0 };
    auxv[n_auxv++] = (struct auxv_pair){ 17 /* AT_CLKTCK */, 100 };
    auxv[n_auxv++] = (struct auxv_pair){ 14 /* AT_EGID */,   0 };
    auxv[n_auxv++] = (struct auxv_pair){ 13 /* AT_GID */,    0 };
    auxv[n_auxv++] = (struct auxv_pair){ 12 /* AT_EUID */,   0 };
    auxv[n_auxv++] = (struct auxv_pair){ 11 /* AT_UID */,    0 };
    auxv[n_auxv++] = (struct auxv_pair){ 9  /* AT_ENTRY */,  load_base + eh->e_entry };
    auxv[n_auxv++] = (struct auxv_pair){ 8  /* AT_FLAGS */,  0 };
    auxv[n_auxv++] = (struct auxv_pair){ 7  /* AT_BASE */,   0 };
    auxv[n_auxv++] = (struct auxv_pair){ 6  /* AT_PAGESZ */, PAGE_SIZE };
    auxv[n_auxv++] = (struct auxv_pair){ 5  /* AT_PHNUM */,  eh->e_phnum };
    auxv[n_auxv++] = (struct auxv_pair){ 4  /* AT_PHENT */,  eh->e_phentsize };
    auxv[n_auxv++] = (struct auxv_pair){ 3  /* AT_PHDR */,   phdr_va };
    auxv[n_auxv++] = (struct auxv_pair){ 0  /* AT_NULL */,   0 };

    /* Calculate total pointer quads:
       argc (1) + argv pointers (argc) + NULL (1) + envp pointers (envc) + NULL (1) + auxv (2 * n_auxv) */
    uint64_t total_quads = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + (2ULL * (uint64_t)n_auxv);
    if ((cur_sp - total_quads * 8ULL) % 16ULL != 0) {
        cur_sp -= 8; /* 16-byte align final rsp */
    }

    /* Push AUXV */
    for (int i = n_auxv - 1; i >= 0; i--) {
        cur_sp -= 8;
        *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = auxv[i].val;
        cur_sp -= 8;
        *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = auxv[i].type;
    }

    /* Push envp NULL terminator */
    cur_sp -= 8;
    *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = 0;

    /* Push envp pointers */
    for (int i = envc - 1; i >= 0; i--) {
        cur_sp -= 8;
        *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = env_vaddrs[i];
    }

    /* Push argv NULL terminator */
    cur_sp -= 8;
    *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = 0;

    /* Push argv pointers */
    for (int i = argc - 1; i >= 0; i--) {
        cur_sp -= 8;
        *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = str_vaddrs[i];
    }

    /* Push argc */
    cur_sp -= 8;
    *(uint64_t*)(page_ptr + (cur_sp - top_page_vaddr)) = (uint64_t) argc;

    *out_entry = load_base + eh->e_entry;
    *out_stack_top = cur_sp;
    if (out_vmas) *out_vmas = vmas;
    return 0;
}
