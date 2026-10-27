#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <agent.h>
#include <sys/myos.h>

/* Standalone SHA-256 implementation for userland audit log verification */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} test_sha256_ctx_t;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define EP1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SIG0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void test_sha256_transform(test_sha256_ctx_t* ctx, const uint8_t* data) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void test_sha256_init(test_sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void test_sha256_update(test_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    while (len > 0) {
        size_t take = (64 - idx < len) ? (64 - idx) : len;
        memcpy(&ctx->buffer[idx], data, take);
        idx += take; data += take; len -= take;
        if (idx == 64) {
            test_sha256_transform(ctx, ctx->buffer);
            idx = 0;
        }
    }
}

static void test_sha256_final(test_sha256_ctx_t* ctx, uint8_t* hash) {
    uint64_t total_bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(&ctx->buffer[idx], 0, 64 - idx);
        test_sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    memset(&ctx->buffer[idx], 0, 56 - idx);
    for (int i = 7; i >= 0; i--) {
        ctx->buffer[56 + (7 - i)] = (uint8_t)(total_bits >> (i * 8));
    }
    test_sha256_transform(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void hex_to_bin(const char* hex, uint8_t* bin, size_t bin_len) {
    for (size_t i = 0; i < bin_len; i++) {
        unsigned int byte_val = 0;
        sscanf(hex + i * 2, "%02x", &byte_val);
        bin[i] = (uint8_t)byte_val;
    }
}

static void bin_to_hex(const uint8_t* bin, size_t bin_len, char* hex) {
    const char hc[] = "0123456789abcdef";
    for (size_t i = 0; i < bin_len; i++) {
        hex[i * 2] = hc[(bin[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = hc[bin[i] & 0x0f];
    }
    hex[bin_len * 2] = '\0';
}

/* 1. Capability allowlist test */
static int test_capabilities(void) {
    printf("[TEST 1] Testing capabilities and path allowlists...\n");

    /* Create sample file in /home */
    int tfd = open("/home/test_read.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (tfd >= 0) {
        write(tfd, "AGENT_SECRET_DATA\n", 18);
        close(tfd);
    }

    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = CAP_FS_READ | CAP_EXEC;
    caps.num_fs_read = 1;
    strncpy(caps.fs_read[0].prefix, "/home", MAX_PATH_PREFIX_LEN - 1);
    caps.num_exec = 1;
    strncpy(caps.exec[0].prefix, "/bin", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));

    int ag_id = agent_create("cap_test_agent", &caps, &budget);
    if (ag_id < 0) {
        printf("FAILED: agent_create failed\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* In child: attach to agent */
        if (agent_attach(ag_id) < 0) {
            perror("agent_attach");
            exit(1);
        }

        /* 1a. Reading /home/test_read.txt should succeed */
        int fd = open("/home/test_read.txt", O_RDONLY);
        if (fd < 0) {
            printf("FAILED: reading /home/test_read.txt failed (errno %d)\n", errno);
            exit(2);
        }
        char rdbuf[32];
        int n = read(fd, rdbuf, sizeof(rdbuf) - 1);
        close(fd);
        if (n <= 0) {
            printf("FAILED: read returned %d\n", n);
            exit(3);
        }

        /* 1b. Reading /etc/init.conf should fail with EACCES */
        fd = open("/etc/init.conf", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            printf("FAILED: reading /etc/init.conf unexpectedly succeeded\n");
            exit(4);
        }
        if (errno != EACCES) {
            printf("FAILED: reading /etc/init.conf gave errno %d, expected EACCES\n", errno);
            exit(5);
        }

        /* 1c. Writing anywhere should fail with EACCES */
        fd = open("/home/test_write.txt", O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) {
            close(fd);
            printf("FAILED: writing to /home/test_write.txt unexpectedly succeeded\n");
            exit(6);
        }
        if (errno != EACCES) {
            printf("FAILED: write to /home gave errno %d, expected EACCES\n", errno);
            exit(7);
        }

        fd = open("/tmp/agent_test.txt", O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) {
            close(fd);
            printf("FAILED: writing to /tmp unexpectedly succeeded\n");
            exit(8);
        }

        /* Denied unlink */
        if (unlink("/home/test_read.txt") == 0) {
            printf("FAILED: unlink unexpectedly succeeded\n");
            exit(9);
        }

        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    agent_kill(ag_id);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASSED: Capability allowlist and denial enforcement OK\n");
        return 1;
    } else {
        printf("FAILED: Child exited with status 0x%x\n", status);
        return 0;
    }
}

/* 2. Delegation narrowing test */
static int test_delegation(void) {
    printf("[TEST 2] Testing capability delegation narrowing (cannot widen)...\n");

    agent_caps_t parent_caps;
    memset(&parent_caps, 0, sizeof(parent_caps));
    parent_caps.mask = CAP_FS_READ | CAP_SPAWN;
    parent_caps.num_fs_read = 1;
    strncpy(parent_caps.fs_read[0].prefix, "/home/sub", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));

    int parent_id = agent_create("parent_agent", &parent_caps, &budget);
    if (parent_id < 0) {
        printf("FAILED: agent_create parent failed\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(parent_id) < 0) exit(1);

        /* Attempt 1: Add CAP_FS_WRITE (widening mask) -> must fail */
        agent_caps_t child_caps;
        memset(&child_caps, 0, sizeof(child_caps));
        child_caps.mask = CAP_FS_READ | CAP_FS_WRITE;
        child_caps.num_fs_read = 1;
        strncpy(child_caps.fs_read[0].prefix, "/home/sub", MAX_PATH_PREFIX_LEN - 1);
        int cid = agent_create("child_widen_mask", &child_caps, &budget);
        if (cid > 0) {
            printf("FAILED: child agent widened capability mask!\n");
            exit(2);
        }
        if (errno != EACCES) {
            printf("FAILED: widening mask gave errno %d, expected EACCES\n", errno);
            exit(3);
        }

        /* Attempt 2: Widen path prefix to /home -> must fail */
        memset(&child_caps, 0, sizeof(child_caps));
        child_caps.mask = CAP_FS_READ;
        child_caps.num_fs_read = 1;
        strncpy(child_caps.fs_read[0].prefix, "/home", MAX_PATH_PREFIX_LEN - 1);
        cid = agent_create("child_widen_prefix", &child_caps, &budget);
        if (cid > 0) {
            printf("FAILED: child agent widened path prefix!\n");
            exit(4);
        }
        if (errno != EACCES) {
            printf("FAILED: widening prefix gave errno %d, expected EACCES\n", errno);
            exit(5);
        }

        /* Attempt 3: Valid narrowing to /home/sub/deep -> must SUCCEED */
        memset(&child_caps, 0, sizeof(child_caps));
        child_caps.mask = CAP_FS_READ;
        child_caps.num_fs_read = 1;
        strncpy(child_caps.fs_read[0].prefix, "/home/sub/deep", MAX_PATH_PREFIX_LEN - 1);
        cid = agent_create("child_narrow", &child_caps, &budget);
        if (cid <= 0) {
            printf("FAILED: valid delegation narrowing was rejected (errno %d)!\n", errno);
            exit(6);
        }

        agent_kill(cid);
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    agent_kill(parent_id);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASSED: Delegation narrowing enforced OK\n");
        return 1;
    } else {
        printf("FAILED: Delegation test exited with status 0x%x\n", status);
        return 0;
    }
}

/* 3. Budget CPU limit test */
static int test_cpu_budget(void) {
    printf("[TEST 3] Testing CPU budget limit suspension...\n");

    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = 0xFFFFFFFF; /* full caps */

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    budget.max_cpu_ticks = 15; /* 15 ticks ~ 150ms */

    int ag_id = agent_create("cpu_spin_agent", &caps, &budget);
    if (ag_id < 0) {
        printf("FAILED: agent_create failed\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(ag_id) < 0) exit(1);
        /* Spin CPU */
        volatile uint32_t count = 0;
        for (;;) {
            count++;
        }
    }

    /* Parent sleeps and checks status */
    struct timespec req = { 0, 300000000 }; /* 300ms */
    nanosleep(&req, NULL);

    agent_info_t info;
    if (agent_info(ag_id, &info) != 0) {
        printf("FAILED: agent_info failed\n");
        agent_kill(ag_id);
        return 0;
    }

    printf("  Agent %d state: %d (ticks: %u/%u)\n",
           ag_id, info.state, info.usage.cpu_ticks, info.budget.max_cpu_ticks);

    if (info.state != AGENT_STATE_SUSPENDED) {
        printf("FAILED: Agent was not suspended when CPU limit reached!\n");
        agent_kill(ag_id);
        return 0;
    }

    agent_kill(ag_id);
    int status = 0;
    waitpid(pid, &status, 0);

    printf("PASSED: CPU budget suspended runaway agent cleanly\n");
    return 1;
}

/* 4. Fork storm max_procs test */
static int test_max_procs(void) {
    printf("[TEST 4] Testing max_procs budget...\n");

    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = 0xFFFFFFFF;

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    budget.max_procs = 2; /* At most 2 active processes */

    int ag_id = agent_create("proc_storm_agent", &caps, &budget);
    if (ag_id < 0) {
        printf("FAILED: agent_create failed\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(ag_id) < 0) exit(1);

        /* Process 1 is current child. Now fork Process 2 */
        pid_t p2 = fork();
        if (p2 == 0) {
            /* Child 2 */
            struct timespec req = { 0, 200000000 };
            nanosleep(&req, NULL);
            exit(0);
        } else if (p2 > 0) {
            /* Now try to fork Process 3: MUST FAIL because max_procs == 2 */
            pid_t p3 = fork();
            if (p3 > 0) {
                printf("FAILED: 3rd process fork succeeded but max_procs=2!\n");
                kill(p3, SIGKILL);
                kill(p2, SIGKILL);
                exit(2);
            }
            if (p3 < 0) {
                /* Fork correctly rejected */
                waitpid(p2, NULL, 0);
                exit(0);
            }
        } else {
            printf("FAILED: Process 2 fork failed unexpectedly\n");
            exit(3);
        }
    }

    int status = 0;
    waitpid(pid, &status, 0);
    agent_kill(ag_id);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASSED: max_procs prevented fork storm\n");
        return 1;
    } else {
        printf("FAILED: Fork storm test exited with status 0x%x\n", status);
        return 0;
    }
}

/* 5. Audit log SHA-256 hash-chain verification */
static int test_audit_log_chain(void) {
    printf("[TEST 5] Recomputing and verifying audit log SHA-256 hash chain...\n");

    int fd = open("/proc/agents/log", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/agents/log");
        return 0;
    }

    char* buf = (char*)malloc(65536);
    if (!buf) { close(fd); return 0; }

    int total = 0;
    for (;;) {
        int n = read(fd, buf + total, 65535 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(fd);

    if (total == 0) {
        printf("FAILED: /proc/agents/log is empty!\n");
        free(buf);
        return 0;
    }

    /* Iterate over lines */
    char* line = buf;
    int entry_count = 0;
    uint8_t expected_prev_hash[32];
    memset(expected_prev_hash, 0, 32);

    while (*line) {
        char* next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';

        if (strncmp(line, "ENTRY ", 6) == 0) {
            uint32_t seq = 0, agent_id = 0, tick = 0, wall_time = 0, ev_type = 0;
            char detail_buf[64] = {0};
            char prev_hex[65] = {0};
            char hash_hex[65] = {0};

            /* Extract fields */
            char* p = strstr(line, "seq=");
            if (p) seq = (uint32_t)strtoul(p + 4, NULL, 10);
            p = strstr(line, "id=");
            if (p) agent_id = (uint32_t)strtoul(p + 3, NULL, 10);
            p = strstr(line, "tick=");
            if (p) tick = (uint32_t)strtoul(p + 5, NULL, 10);
            p = strstr(line, "time=");
            if (p) wall_time = (uint32_t)strtoul(p + 5, NULL, 10);
            p = strstr(line, "type=");
            if (p) ev_type = (uint32_t)strtoul(p + 5, NULL, 10);

            p = strstr(line, "detail=\"");
            if (p) {
                char* d_end = strchr(p + 8, '\"');
                if (d_end) {
                    size_t dlen = d_end - (p + 8);
                    if (dlen > sizeof(detail_buf) - 1) dlen = sizeof(detail_buf) - 1;
                    memcpy(detail_buf, p + 8, dlen);
                    detail_buf[dlen] = '\0';
                }
            }

            p = strstr(line, "prev=");
            if (p) {
                strncpy(prev_hex, p + 5, 64);
                prev_hex[64] = '\0';
            }

            p = strstr(line, "hash=");
            if (p) {
                strncpy(hash_hex, p + 5, 64);
                hash_hex[64] = '\0';
            }

            uint8_t prev_bin[32];
            uint8_t hash_bin[32];
            hex_to_bin(prev_hex, prev_bin, 32);
            hex_to_bin(hash_hex, hash_bin, 32);

            /* Check that prev_bin matches previous entry's hash (if not first entry) */
            if (entry_count > 0 && memcmp(prev_bin, expected_prev_hash, 32) != 0) {
                printf("FAILED: entry %u prev_hash mismatch with previous hash!\n", seq);
                free(buf);
                return 0;
            }

            /* Reconstruct audit_payload_t */
            audit_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            payload.seq = seq;
            payload.agent_id = agent_id;
            payload.tick = tick;
            payload.wall_time = wall_time;
            payload.event_type = ev_type;
            strncpy(payload.detail, detail_buf, sizeof(payload.detail) - 1);

            /* Calculate SHA256(prev_bin || payload) */
            test_sha256_ctx_t ctx;
            test_sha256_init(&ctx);
            test_sha256_update(&ctx, prev_bin, 32);
            test_sha256_update(&ctx, (const uint8_t*)&payload, sizeof(payload));
            uint8_t calc_hash[32];
            test_sha256_final(&ctx, calc_hash);

            if (memcmp(calc_hash, hash_bin, 32) != 0) {
                char calc_hex[65];
                bin_to_hex(calc_hash, 32, calc_hex);
                printf("FAILED: Entry seq %u hash mismatch!\n  Expected: %s\n  Computed: %s\n",
                       seq, hash_hex, calc_hex);
                free(buf);
                return 0;
            }

            memcpy(expected_prev_hash, hash_bin, 32);
            entry_count++;
        }

        if (!next_line) break;
        line = next_line + 1;
    }

    free(buf);
    printf("PASSED: Verified %d audit log entries, SHA-256 hash chain 100%% valid\n", entry_count);
    return 1;
}

/* 6. Physical frame leak test */
static int test_physical_frame_leaks(void) {
    printf("[TEST 6] Testing physical frame leaks across agent teardown...\n");

    /* Stabilize and record baseline frames */
    sync();
    struct timespec req = { 0, 100000000 };
    nanosleep(&req, NULL);

    uint32_t frames_before = sys_free_frames();

    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = 0xFFFFFFFF;
    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));

    int ag_id = agent_create("leak_probe", &caps, &budget);
    if (ag_id < 0) {
        printf("FAILED: agent_create failed\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(ag_id) < 0) exit(1);
        /* Do some heap allocations */
        void* p1 = malloc(4096 * 4);
        if (p1) memset(p1, 0xAA, 4096 * 4);
        void* p2 = malloc(4096 * 8);
        if (p2) memset(p2, 0xBB, 4096 * 8);
        free(p1);
        free(p2);
        exit(0);
    }

    waitpid(pid, NULL, 0);
    agent_kill(ag_id);

    nanosleep(&req, NULL);
    uint32_t frames_after = sys_free_frames();

    printf("  Frames before: %u, Frames after: %u\n", frames_before, frames_after);

    if (frames_after < frames_before) {
        uint32_t leaked = frames_before - frames_after;
        printf("FAILED: Physical frame leak detected (%u frames leaked)!\n", leaked);
        return 0;
    }

    printf("PASSED: Zero physical frame leaks after agent teardown\n");
    return 1;
}

int main(void) {
    printf("=== MyOS Kernel Agent Test Suite ===\n\n");

    int passes = 0;
    int total = 6;

    if (test_capabilities()) passes++;
    if (test_delegation()) passes++;
    if (test_cpu_budget()) passes++;
    if (test_max_procs()) passes++;
    if (test_audit_log_chain()) passes++;
    if (test_physical_frame_leaks()) passes++;

    printf("\n=== Summary: %d / %d tests passed ===\n", passes, total);
    if (passes == total) {
        printf("ALL AGENT TESTS PASSED!\n");
        return 0;
    }
    return 1;
}
