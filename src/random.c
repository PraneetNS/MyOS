#include "random.h"
#include "sha256.h"
#include "timer.h"
#include "rtc.h"
#include "vfs.h"
#include "bcache.h"
#include "kmutex.h"
#include "console.h"
#include "serial.h"
#include <string.h>

#define ROTL(a,b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); \
    c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  \
    c += d; b ^= c; b = ROTL(b, 7);

static uint32_t s_chacha_state[16];
static uint8_t  s_entropy_pool[32];
static uint32_t s_bytes_since_reseed = 0;
static kmutex_t s_random_lock;
static int s_has_rdrand = -1;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int cpu_has_rdrand(void) {
    if (s_has_rdrand != -1) return s_has_rdrand;
    uint32_t eax, ebx, ecx, edx;
    asm volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    s_has_rdrand = (ecx & (1 << 30)) ? 1 : 0;
    return s_has_rdrand;
}

static int get_rdrand_u32(uint32_t* val) {
    if (!cpu_has_rdrand()) return 0;
    uint8_t ok;
    asm volatile ("rdrand %0; setc %1" : "=r"(*val), "=qm"(ok));
    return ok;
}

static void chacha_block(uint32_t out[16], const uint32_t in[16]) {
    for (int i = 0; i < 16; i++) out[i] = in[i];
    for (int i = 0; i < 10; i++) {
        /* Column rounds */
        QR(out[0], out[4], out[8],  out[12]);
        QR(out[1], out[5], out[9],  out[13]);
        QR(out[2], out[6], out[10], out[14]);
        QR(out[3], out[7], out[11], out[15]);
        /* Diagonal rounds */
        QR(out[0], out[5], out[10], out[15]);
        QR(out[1], out[6], out[11], out[12]);
        QR(out[2], out[7], out[8],  out[13]);
        QR(out[3], out[4], out[9],  out[14]);
    }
    for (int i = 0; i < 16; i++) out[i] += in[i];
}

void random_add_entropy(const void* data, size_t len) {
    if (!data || len == 0) return;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, s_entropy_pool, sizeof(s_entropy_pool));
    sha256_update(&ctx, (const uint8_t*)data, len);
    uint64_t tsc = rdtsc();
    sha256_update(&ctx, (const uint8_t*)&tsc, sizeof(tsc));
    sha256_final(&ctx, s_entropy_pool);
}

static void random_reseed_internal(void) {
    uint32_t hw_rand[4] = {0};
    for (int i = 0; i < 4; i++) {
        if (!get_rdrand_u32(&hw_rand[i])) {
            hw_rand[i] = (uint32_t) rdtsc() ^ (timer_get_ticks() << 16);
        }
    }
    random_add_entropy(hw_rand, sizeof(hw_rand));

    /* "expand 32-byte k" */
    s_chacha_state[0] = 0x61707865;
    s_chacha_state[1] = 0x3320646e;
    s_chacha_state[2] = 0x79622d32;
    s_chacha_state[3] = 0x6b206574;

    /* 256-bit key from entropy pool */
    for (int i = 0; i < 8; i++) {
        uint32_t k = (uint32_t)s_entropy_pool[i * 4] |
                     ((uint32_t)s_entropy_pool[i * 4 + 1] << 8) |
                     ((uint32_t)s_entropy_pool[i * 4 + 2] << 16) |
                     ((uint32_t)s_entropy_pool[i * 4 + 3] << 24);
        s_chacha_state[4 + i] = k;
    }

    s_chacha_state[12] = 0; /* Counter */
    uint64_t tsc = rdtsc();
    s_chacha_state[13] = (uint32_t) tsc;
    s_chacha_state[14] = (uint32_t) (tsc >> 32);
    s_chacha_state[15] = timer_get_ticks();

    s_bytes_since_reseed = 0;
}

void random_init(void) {
    kmutex_init(&s_random_lock, "random");
    memset(s_entropy_pool, 0, sizeof(s_entropy_pool));

    /* Collect initial entropy */
    uint64_t initial_entropy[8];
    initial_entropy[0] = rdtsc();
    initial_entropy[1] = (uint64_t) rtc_get_epoch() << 32 | timer_get_ticks();
    initial_entropy[2] = rdtsc() ^ 0x55AA55AA12345678ULL;
    initial_entropy[3] = (uintptr_t)&s_random_lock;
    for (int i = 4; i < 8; i++) {
        uint32_t r = 0;
        get_rdrand_u32(&r);
        initial_entropy[i] = ((uint64_t)r << 32) | (uint32_t)rdtsc();
    }
    random_add_entropy(initial_entropy, sizeof(initial_entropy));

    /* Try reading /var/lib/random-seed if FAT16 or VFS is ready */
    vnode_t* vn = NULL;
    if (vfs_resolve_path("/var/lib/random-seed", "/", &vn) == 0 && vn) {
        open_file_t* of = open_file_alloc(OPEN_FILE_VNODE, vn, O_RDONLY);
        if (of) {
            uint8_t seed[32];
            int r = vfs_read(of, seed, sizeof(seed));
            vfs_close(of);
            if (r > 0) {
                random_add_entropy(seed, r);
                kprintf("[random] Loaded entropy seed from /var/lib/random-seed\n");
            }
        }
        vnode_unref(vn);
    }

    random_reseed_internal();
    kprintf("[random] ChaCha20 CSPRNG initialized (RDRAND=%s)\n",
            cpu_has_rdrand() ? "yes" : "no");
}

void random_get_bytes(void* buf, size_t len) {
    if (!buf || len == 0) return;
    uint8_t* out = (uint8_t*) buf;

    kmutex_lock(&s_random_lock);

    while (len > 0) {
        if (s_bytes_since_reseed >= 65536) {
            random_reseed_internal();
        }

        uint32_t block[16];
        chacha_block(block, s_chacha_state);
        s_chacha_state[12]++;
        if (s_chacha_state[12] == 0) {
            s_chacha_state[13]++;
        }

        size_t take = (len < 64) ? len : 64;
        memcpy(out, block, take);
        out += take;
        len -= take;
        s_bytes_since_reseed += take;
    }

    kmutex_unlock(&s_random_lock);
}

uint32_t random_u32(void) {
    uint32_t val = 0;
    random_get_bytes(&val, sizeof(val));
    return val;
}

void random_save_seed(void) {
    uint8_t seed[32];
    random_get_bytes(seed, sizeof(seed));

    vnode_t* root = vfs_get_root();
    if (!root) return;

    /* Ensure /var exists */
    vnode_t* var_vn = NULL;
    if (vfs_resolve_path("/var", "/", &var_vn) != 0 || !var_vn) {
        if (root->ops && root->ops->mkdir) {
            vnode_t* created_var = NULL;
            root->ops->mkdir(root, "var", &created_var);
            if (created_var) vnode_unref(created_var);
        }
        vfs_resolve_path("/var", "/", &var_vn);
    }

    /* Ensure /var/lib exists */
    vnode_t* lib_vn = NULL;
    if (var_vn) {
        if (vfs_resolve_path("/var/lib", "/", &lib_vn) != 0 || !lib_vn) {
            if (var_vn->ops && var_vn->ops->mkdir) {
                vnode_t* created_lib = NULL;
                var_vn->ops->mkdir(var_vn, "lib", &created_lib);
                if (created_lib) vnode_unref(created_lib);
            }
            vfs_resolve_path("/var/lib", "/", &lib_vn);
        }
        vnode_unref(var_vn);
    }

    if (!lib_vn) return;

    vnode_t* seed_vn = NULL;
    if (vfs_resolve_path("/var/lib/random-seed", "/", &seed_vn) != 0 || !seed_vn) {
        if (lib_vn->ops && lib_vn->ops->create) {
            lib_vn->ops->create(lib_vn, "random-seed", &seed_vn);
        }
    }
    vnode_unref(lib_vn);

    if (seed_vn) {
        open_file_t* of = open_file_alloc(OPEN_FILE_VNODE, seed_vn, O_WRONLY);
        if (of) {
            vfs_write(of, seed, sizeof(seed));
            vfs_close(of);
            bcache_sync();
            kprintf("[random] Saved fresh seed to /var/lib/random-seed\n");
        }
        vnode_unref(seed_vn);
    }
}
