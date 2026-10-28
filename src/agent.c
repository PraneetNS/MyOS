#include "agent.h"
#include "kmutex.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "rtc.h"
#include "sha256.h"
#include "vfs.h"
#include "errno.h"
#include "serial.h"
#include "vga.h"
#include "signal.h"
#include "kheap.h"

static size_t kstrlen(const char* s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static int kstrncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s1[i] != (unsigned char)s2[i])
            return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

static void kstrncpy(char* dest, const char* src, size_t n) {
    if (!dest || n == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < n) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static agent_t s_agents[MAX_AGENTS];
static kmutex_t s_agent_lock;

/* Audit log ring buffer */
static audit_entry_t s_audit_entries[MAX_AUDIT_ENTRIES];
static uint32_t s_audit_head = 0;
static uint32_t s_audit_count = 0;
static uint32_t s_audit_seq = 1;
static uint8_t  s_current_hash[32];

static const char* const s_event_names[] = {
    "UNKNOWN",
    "SYSCALL_ALLOWED_SENSITIVE",
    "DENIED",
    "BUDGET_EXCEEDED",
    "SPAWN",
    "KILL",
    "APPROVAL_REQUEST",
    "APPROVAL_GRANT",
    "APPROVAL_DENY",
    "BUS_REGISTER",
    "BUS_CALL",
    "BUS_DENIED",
    "BUS_APPROVAL"
};

const char* agent_event_name(uint32_t event_type) {
    if (event_type >= sizeof(s_event_names) / sizeof(s_event_names[0]))
        return "UNKNOWN";
    return s_event_names[event_type];
}

static void bin_to_hex(const uint8_t* bin, size_t len, char* hex) {
    const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex[i * 2]     = hex_chars[(bin[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bin[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

void agent_init(void) {
    kmutex_init(&s_agent_lock, "agent");
    for (int i = 0; i < MAX_AGENTS; i++) {
        memset(&s_agents[i], 0, sizeof(agent_t));
        s_agents[i].id = i + 1;
        s_agents[i].state = AGENT_STATE_UNUSED;
    }
    memset(s_audit_entries, 0, sizeof(s_audit_entries));
    memset(s_current_hash, 0, sizeof(s_current_hash));
    s_audit_head = 0;
    s_audit_count = 0;
    s_audit_seq = 1;
}

agent_t* agent_get_by_id(int id) {
    if (id < 1 || id > MAX_AGENTS) return NULL;
    agent_t* ag = &s_agents[id - 1];
    if (!ag->in_use) return NULL;
    return ag;
}

int agent_is_runnable(int agent_id) {
    if (agent_id <= 0) return 1;
    if (agent_id > MAX_AGENTS) return 0;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return 0;
    return (ag->state == AGENT_STATE_RUNNING);
}

void agent_audit_log(int agent_id, uint32_t event_type, const char* detail) {
    kmutex_lock(&s_agent_lock);

    audit_entry_t* entry = &s_audit_entries[s_audit_head];
    memcpy(entry->prev_hash, s_current_hash, 32);

    entry->payload.seq = s_audit_seq++;
    entry->payload.agent_id = (uint32_t)agent_id;
    entry->payload.tick = timer_get_ticks();
    entry->payload.wall_time = rtc_get_epoch();
    entry->payload.event_type = event_type;

    memset(entry->payload.detail, 0, sizeof(entry->payload.detail));
    if (detail) {
        int i = 0;
        while (detail[i] && i < (int)(sizeof(entry->payload.detail) - 1)) {
            entry->payload.detail[i] = detail[i];
            i++;
        }
        entry->payload.detail[i] = '\0';
    }

    /* Compute hash: SHA256(prev_hash || payload) */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, entry->prev_hash, 32);
    sha256_update(&ctx, (const uint8_t*)&entry->payload, sizeof(audit_payload_t));
    sha256_final(&ctx, entry->hash);

    memcpy(s_current_hash, entry->hash, 32);

    s_audit_head = (s_audit_head + 1) % MAX_AUDIT_ENTRIES;
    if (s_audit_count < MAX_AUDIT_ENTRIES)
        s_audit_count++;

    kmutex_unlock(&s_agent_lock);
}

static int path_matches_prefix(const char* path, const char* prefix) {
    if (!path || !prefix || !prefix[0]) return 0;
    if (prefix[0] == '/' && prefix[1] == '\0') return 1; /* "/" covers all */
    size_t plen = kstrlen(prefix);
    if (kstrncmp(path, prefix, plen) != 0) return 0;
    if (path[plen] == '\0' || path[plen] == '/' || prefix[plen - 1] == '/' || prefix[plen - 1] == '.') return 1;
    return 0;
}

int agent_create_kernel(const char* name, const agent_caps_t* caps, const agent_budget_t* budget, int owner_pid, int parent_agent_id) {
    if (!name || !caps || !budget) return -EINVAL;

    kmutex_lock(&s_agent_lock);

    /* Delegation check: if created by an agent, verify CAP_SPAWN and narrowing */
    if (parent_agent_id > 0) {
        agent_t* parent = agent_get_by_id(parent_agent_id);
        if (!parent || parent->state != AGENT_STATE_RUNNING) {
            kmutex_unlock(&s_agent_lock);
            return -EACCES;
        }

        if (!(parent->caps.mask & CAP_SPAWN)) {
            kmutex_unlock(&s_agent_lock);
            agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: missing CAP_SPAWN");
            kprintf("[agent %d] DENIED: agent_create missing CAP_SPAWN\n", parent_agent_id);
            return -EACCES;
        }

        /* Child mask must be subset of parent mask */
        if ((caps->mask & ~parent->caps.mask) != 0) {
            kmutex_unlock(&s_agent_lock);
            agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: bitmask widening attempted");
            kprintf("[agent %d] DENIED: capability bitmask widening attempted\n", parent_agent_id);
            return -EACCES;
        }

        /* Check that child prefixes are sub-prefixes of parent's allowed prefixes */
        for (uint32_t i = 0; i < caps->num_fs_read && i < MAX_PATH_RULES; i++) {
            int ok = 0;
            for (uint32_t j = 0; j < parent->caps.num_fs_read && j < MAX_PATH_RULES; j++) {
                if (path_matches_prefix(caps->fs_read[i].prefix, parent->caps.fs_read[j].prefix)) {
                    ok = 1; break;
                }
            }
            if (!ok) {
                kmutex_unlock(&s_agent_lock);
                agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: fs_read widening attempted");
                kprintf("[agent %d] DENIED: fs_read prefix '%s' widening attempted\n", parent_agent_id, caps->fs_read[i].prefix);
                return -EACCES;
            }
        }

        for (uint32_t i = 0; i < caps->num_fs_write && i < MAX_PATH_RULES; i++) {
            int ok = 0;
            for (uint32_t j = 0; j < parent->caps.num_fs_write && j < MAX_PATH_RULES; j++) {
                if (path_matches_prefix(caps->fs_write[i].prefix, parent->caps.fs_write[j].prefix)) {
                    ok = 1; break;
                }
            }
            if (!ok) {
                kmutex_unlock(&s_agent_lock);
                agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: fs_write widening attempted");
                kprintf("[agent %d] DENIED: fs_write prefix '%s' widening attempted\n", parent_agent_id, caps->fs_write[i].prefix);
                return -EACCES;
            }
        }

        for (uint32_t i = 0; i < caps->num_exec && i < MAX_PATH_RULES; i++) {
            int ok = 0;
            for (uint32_t j = 0; j < parent->caps.num_exec && j < MAX_PATH_RULES; j++) {
                if (path_matches_prefix(caps->exec[i].prefix, parent->caps.exec[j].prefix)) {
                    ok = 1; break;
                }
            }
            if (!ok) {
                kmutex_unlock(&s_agent_lock);
                agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: exec widening attempted");
                kprintf("[agent %d] DENIED: exec prefix '%s' widening attempted\n", parent_agent_id, caps->exec[i].prefix);
                return -EACCES;
            }
        }

        for (uint32_t i = 0; i < caps->num_connect && i < MAX_PATH_RULES; i++) {
            int ok = 0;
            for (uint32_t j = 0; j < parent->caps.num_connect && j < MAX_PATH_RULES; j++) {
                if (path_matches_prefix(caps->connect[i].prefix, parent->caps.connect[j].prefix)) {
                    ok = 1; break;
                }
            }
            if (!ok) {
                kmutex_unlock(&s_agent_lock);
                agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: connect widening attempted");
                kprintf("[agent %d] DENIED: connect prefix '%s' widening attempted\n", parent_agent_id, caps->connect[i].prefix);
                return -EACCES;
            }
        }

        for (uint32_t i = 0; i < caps->num_bus_call && i < MAX_PATH_RULES; i++) {
            int ok = 0;
            for (uint32_t j = 0; j < parent->caps.num_bus_call && j < MAX_PATH_RULES; j++) {
                if (path_matches_prefix(caps->bus_call[i].prefix, parent->caps.bus_call[j].prefix)) {
                    ok = 1; break;
                }
            }
            if (!ok) {
                kmutex_unlock(&s_agent_lock);
                agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: bus_call widening attempted");
                kprintf("[agent %d] DENIED: bus_call prefix '%s' widening attempted\n", parent_agent_id, caps->bus_call[i].prefix);
                return -EACCES;
            }
        }

        for (uint32_t i = 0; i < caps->num_bus_provide && i < MAX_PATH_RULES; i++) {
            int ok = 0;
            for (uint32_t j = 0; j < parent->caps.num_bus_provide && j < MAX_PATH_RULES; j++) {
                if (path_matches_prefix(caps->bus_provide[i].prefix, parent->caps.bus_provide[j].prefix)) {
                    ok = 1; break;
                }
            }
            if (!ok) {
                kmutex_unlock(&s_agent_lock);
                agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: bus_provide widening attempted");
                kprintf("[agent %d] DENIED: bus_provide prefix '%s' widening attempted\n", parent_agent_id, caps->bus_provide[i].prefix);
                return -EACCES;
            }
        }

        /* Check model_backends narrowing */
        uint32_t p_mb = parent->caps.model_backends ? parent->caps.model_backends : MODEL_BACKEND_ALL;
        uint32_t c_mb = caps->model_backends ? caps->model_backends : p_mb;
        if ((c_mb & ~p_mb) != 0) {
            kmutex_unlock(&s_agent_lock);
            agent_audit_log(parent_agent_id, AUDIT_EVENT_DENIED, "agent_create: model_backends widening attempted");
            kprintf("[agent %d] DENIED: model_backends widening attempted\n", parent_agent_id);
            return -EACCES;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!s_agents[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        kmutex_unlock(&s_agent_lock);
        return -ENOMEM;
    }

    agent_t* ag = &s_agents[slot];
    memset(ag, 0, sizeof(agent_t));
    ag->id = slot + 1;
    kstrncpy(ag->name, name, AGENT_NAME_MAX - 1);
    ag->name[AGENT_NAME_MAX - 1] = '\0';
    ag->owner_pid = owner_pid;
    ag->parent_agent_id = parent_agent_id;
    ag->state = AGENT_STATE_RUNNING;
    ag->caps = *caps;
    if (ag->caps.model_backends == 0) {
        if (parent_agent_id > 0) {
            agent_t* parent = agent_get_by_id(parent_agent_id);
            ag->caps.model_backends = parent ? parent->caps.model_backends : MODEL_BACKEND_ALL;
        } else {
            ag->caps.model_backends = MODEL_BACKEND_ALL;
        }
    }
    ag->budget = *budget;
    ag->usage.cpu_ticks = 0;
    ag->usage.mem_pages = 0;
    ag->usage.procs_count = 0;
    ag->usage.tokens_used = 0;
    ag->in_use = 1;

    int new_id = ag->id;
    kmutex_unlock(&s_agent_lock);

    char detail[64];
    detail[0] = '\0';
    int p = 0;
    for (int i = 0; name[i] && p < 40; i++) detail[p++] = name[i];
    detail[p] = '\0';
    agent_audit_log(new_id, AUDIT_EVENT_SPAWN, detail);
    kprintf("[agent %d] Created agent '%s' (owner PID %d)\n", new_id, ag->name, owner_pid);

    return new_id;
}

int agent_attach_proc(int agent_id, process_t* p) {
    if (!p) return -EINVAL;

    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag || ag->state != AGENT_STATE_RUNNING) {
        kmutex_unlock(&s_agent_lock);
        return -EINVAL;
    }

    /* If process was already bound to an agent, decrement previous agent's proc count */
    if (p->agent_id > 0 && p->agent_id <= MAX_AGENTS) {
        agent_t* prev = &s_agents[p->agent_id - 1];
        if (prev->in_use && prev->usage.procs_count > 0)
            prev->usage.procs_count--;
    }

    p->agent_id = agent_id;
    ag->usage.procs_count++;
    kmutex_unlock(&s_agent_lock);

    agent_audit_log(agent_id, AUDIT_EVENT_SYSCALL_ALLOWED_SENSITIVE, "attach_proc");
    return 0;
}

int agent_kill_kernel(int agent_id) {
    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag) {
        kmutex_unlock(&s_agent_lock);
        return -EINVAL;
    }

    ag->state = AGENT_STATE_KILLED;
    kmutex_unlock(&s_agent_lock);

    /* Terminate all processes in this agent */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* proc = process_table_entry(i);
        if (proc && proc->state != PROC_UNUSED && proc->state != PROC_ZOMBIE && proc->agent_id == agent_id) {
            sig_send(proc, SIGKILL);
        }
    }

    agent_audit_log(agent_id, AUDIT_EVENT_KILL, "agent_kill");
    kprintf("[agent %d] Killed\n", agent_id);
    return 0;
}

int agent_suspend_kernel(int agent_id) {
    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag) {
        kmutex_unlock(&s_agent_lock);
        return -EINVAL;
    }

    ag->state = AGENT_STATE_SUSPENDED;
    kmutex_unlock(&s_agent_lock);

    agent_audit_log(agent_id, AUDIT_EVENT_SYSCALL_ALLOWED_SENSITIVE, "agent_suspend");
    kprintf("[agent %d] Suspended\n", agent_id);
    return 0;
}

int agent_resume_kernel(int agent_id) {
    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag) {
        kmutex_unlock(&s_agent_lock);
        return -EINVAL;
    }

    ag->state = AGENT_STATE_RUNNING;
    kmutex_unlock(&s_agent_lock);

    agent_audit_log(agent_id, AUDIT_EVENT_SYSCALL_ALLOWED_SENSITIVE, "agent_resume");
    kprintf("[agent %d] Resumed\n", agent_id);
    return 0;
}

int agent_get_info(int agent_id, agent_info_t* info) {
    if (!info) return -EINVAL;

    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag) {
        kmutex_unlock(&s_agent_lock);
        return -ENOENT;
    }

    info->id = ag->id;
    kstrncpy(info->name, ag->name, AGENT_NAME_MAX - 1);
    info->name[AGENT_NAME_MAX - 1] = '\0';
    info->owner_pid = ag->owner_pid;
    info->parent_agent_id = ag->parent_agent_id;
    info->state = ag->state;
    info->caps = ag->caps;
    info->budget = ag->budget;
    info->usage = ag->usage;

    kmutex_unlock(&s_agent_lock);
    return 0;
}

void agent_charge_cpu(int agent_id, uint32_t ticks) {
    if (agent_id <= 0 || agent_id > MAX_AGENTS) return;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return;

    ag->usage.cpu_ticks += ticks;
    if (ag->budget.max_cpu_ticks > 0 && ag->usage.cpu_ticks >= ag->budget.max_cpu_ticks) {
        if (ag->state == AGENT_STATE_RUNNING) {
            ag->state = AGENT_STATE_SUSPENDED;
            agent_audit_log(agent_id, AUDIT_EVENT_BUDGET_EXCEEDED, "cpu limit exceeded; suspended");
            kprintf("[agent %d] BUDGET EXCEEDED: cpu ticks %u >= limit %u; suspended\n",
                    agent_id, ag->usage.cpu_ticks, ag->budget.max_cpu_ticks);
        }
    }
}

int agent_check_mem(int agent_id, uint32_t additional_pages) {
    if (agent_id <= 0 || agent_id > MAX_AGENTS) return 1;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return 1;

    if (ag->budget.max_mem_pages > 0 && ag->usage.mem_pages + additional_pages > ag->budget.max_mem_pages) {
        if (ag->state == AGENT_STATE_RUNNING) {
            ag->state = AGENT_STATE_SUSPENDED;
            agent_audit_log(agent_id, AUDIT_EVENT_BUDGET_EXCEEDED, "mem limit exceeded; suspended");
            kprintf("[agent %d] BUDGET EXCEEDED: mem pages %u + %u > limit %u; suspended\n",
                    agent_id, ag->usage.mem_pages, additional_pages, ag->budget.max_mem_pages);
        }
        return 0;
    }
    return 1;
}

void agent_charge_mem(int agent_id, uint32_t pages) {
    if (agent_id <= 0 || agent_id > MAX_AGENTS) return;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return;
    ag->usage.mem_pages += pages;
}

void agent_uncharge_mem(int agent_id, uint32_t pages) {
    if (agent_id <= 0 || agent_id > MAX_AGENTS) return;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return;
    if (ag->usage.mem_pages >= pages)
        ag->usage.mem_pages -= pages;
    else
        ag->usage.mem_pages = 0;
}

int agent_can_fork(int agent_id) {
    if (agent_id <= 0 || agent_id > MAX_AGENTS) return 1;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return 1;

    if (ag->budget.max_procs > 0 && ag->usage.procs_count >= ag->budget.max_procs) {
        agent_audit_log(agent_id, AUDIT_EVENT_BUDGET_EXCEEDED, "fork denied: max_procs exceeded");
        kprintf("[agent %d] BUDGET EXCEEDED: fork denied, procs %u >= limit %u\n",
                agent_id, ag->usage.procs_count, ag->budget.max_procs);
        return 0;
    }
    ag->usage.procs_count++;
    return 1;
}

void agent_proc_exited(int agent_id) {
    if (agent_id <= 0 || agent_id > MAX_AGENTS) return;
    agent_t* ag = &s_agents[agent_id - 1];
    if (!ag->in_use) return;
    if (ag->usage.procs_count > 0)
        ag->usage.procs_count--;
}

int agent_check_access(process_t* p, uint32_t cap_bit, const char* path, const char* op_name) {
    if (!p || p->agent_id <= 0) return 0; /* Unconfined non-agent process */

    agent_t* ag = agent_get_by_id(p->agent_id);
    if (!ag || ag->state != AGENT_STATE_RUNNING) {
        agent_audit_log(p->agent_id, AUDIT_EVENT_DENIED, "process in non-running agent");
        kprintf("[agent %d] DENIED: %s (agent not running)\n", p->agent_id, op_name ? op_name : "syscall");
        return -EACCES;
    }

    /* Bitmask check */
    if (!(ag->caps.mask & cap_bit)) {
        char detail[64];
        detail[0] = '\0';
        int d = 0;
        const char* s = op_name ? op_name : "op";
        while (*s && d < 30) detail[d++] = *s++;
        detail[d++] = ':'; detail[d++] = ' ';
        detail[d++] = 'n'; detail[d++] = 'o'; detail[d++] = ' '; detail[d++] = 'c'; detail[d++] = 'a'; detail[d++] = 'p';
        detail[d] = '\0';
        agent_audit_log(ag->id, AUDIT_EVENT_DENIED, detail);
        kprintf("[agent %d] DENIED: %s (missing cap bit 0x%x)\n", ag->id, op_name ? op_name : "syscall", cap_bit);
        return -EACCES;
    }

    /* Path prefix allowlist check */
    if (path) {
        char canon[256];
        vfs_path_canonical(path, p->cwd, canon, sizeof(canon));

        int ok = 0;
        if (cap_bit == CAP_FS_READ) {
            for (uint32_t i = 0; i < ag->caps.num_fs_read && i < MAX_PATH_RULES; i++) {
                if (path_matches_prefix(canon, ag->caps.fs_read[i].prefix)) {
                    ok = 1; break;
                }
            }
        } else if (cap_bit == CAP_FS_WRITE) {
            for (uint32_t i = 0; i < ag->caps.num_fs_write && i < MAX_PATH_RULES; i++) {
                if (path_matches_prefix(canon, ag->caps.fs_write[i].prefix)) {
                    ok = 1; break;
                }
            }
        } else if (cap_bit == CAP_EXEC) {
            for (uint32_t i = 0; i < ag->caps.num_exec && i < MAX_PATH_RULES; i++) {
                if (path_matches_prefix(canon, ag->caps.exec[i].prefix)) {
                    ok = 1; break;
                }
            }
        } else if (cap_bit == CAP_SOCKET_CONNECT) {
            for (uint32_t i = 0; i < ag->caps.num_connect && i < MAX_PATH_RULES; i++) {
                if (path_matches_prefix(canon, ag->caps.connect[i].prefix)) {
                    ok = 1; break;
                }
            }
        } else {
            ok = 1;
        }

        if (!ok) {
            char detail[64];
            int d = 0;
            const char* op = op_name ? op_name : "denied";
            while (*op && d < 20) detail[d++] = *op++;
            detail[d++] = ':'; detail[d++] = ' ';
            const char* c = canon;
            while (*c && d < 63) detail[d++] = *c++;
            detail[d] = '\0';
            agent_audit_log(ag->id, AUDIT_EVENT_DENIED, detail);
            kprintf("[agent %d] DENIED: %s on '%s'\n", ag->id, op_name ? op_name : "access", canon);
            return -EACCES;
        }
    }

    return 0;
}

/* Helper to render the entire audit log as text for /proc/agents/log */
static void append_uint_to_buf(char* buf, int* pos, int max, uint32_t val) {
    char tmp[16];
    int i = 0;
    if (val == 0) {
        if (*pos < max - 1) buf[(*pos)++] = '0';
        return;
    }
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        if (*pos < max - 1) buf[(*pos)++] = tmp[--i];
    }
}

static void append_str_to_buf(char* buf, int* pos, int max, const char* s) {
    if (!s) return;
    while (*s && *pos < max - 1) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

int agent_audit_read(uint32_t offset, uint8_t* buf, uint32_t count) {
    kmutex_lock(&s_agent_lock);

    /* Allocate or fill text buffer */
    static char text[65536];
    int pos = 0;

    uint32_t start_idx = (s_audit_head + MAX_AUDIT_ENTRIES - s_audit_count) % MAX_AUDIT_ENTRIES;
    for (uint32_t k = 0; k < s_audit_count; k++) {
        uint32_t idx = (start_idx + k) % MAX_AUDIT_ENTRIES;
        audit_entry_t* e = &s_audit_entries[idx];

        char prev_hex[65];
        char hash_hex[65];
        bin_to_hex(e->prev_hash, 32, prev_hex);
        bin_to_hex(e->hash, 32, hash_hex);

        append_str_to_buf(text, &pos, sizeof(text), "ENTRY seq=");
        append_uint_to_buf(text, &pos, sizeof(text), e->payload.seq);
        append_str_to_buf(text, &pos, sizeof(text), " id=");
        append_uint_to_buf(text, &pos, sizeof(text), e->payload.agent_id);
        append_str_to_buf(text, &pos, sizeof(text), " tick=");
        append_uint_to_buf(text, &pos, sizeof(text), e->payload.tick);
        append_str_to_buf(text, &pos, sizeof(text), " time=");
        append_uint_to_buf(text, &pos, sizeof(text), e->payload.wall_time);
        append_str_to_buf(text, &pos, sizeof(text), " type=");
        append_uint_to_buf(text, &pos, sizeof(text), e->payload.event_type);
        append_str_to_buf(text, &pos, sizeof(text), " detail=\"");
        append_str_to_buf(text, &pos, sizeof(text), e->payload.detail);
        append_str_to_buf(text, &pos, sizeof(text), "\" prev=");
        append_str_to_buf(text, &pos, sizeof(text), prev_hex);
        append_str_to_buf(text, &pos, sizeof(text), " hash=");
        append_str_to_buf(text, &pos, sizeof(text), hash_hex);
        append_str_to_buf(text, &pos, sizeof(text), "\n");
    }

    kmutex_unlock(&s_agent_lock);

    if (offset >= (uint32_t)pos) return 0;
    uint32_t avail = (uint32_t)pos - offset;
    uint32_t to_copy = (avail < count) ? avail : count;
    memcpy(buf, text + offset, to_copy);
    return to_copy;
}

uint32_t agent_audit_size(void) {
    kmutex_lock(&s_agent_lock);
    uint32_t approx = s_audit_count * 200;
    kmutex_unlock(&s_agent_lock);
    return approx;
}

static int s_bus_daemon_pid = -1;
static int s_model_daemon_pid = -1;

static int is_init_tree_process(process_t* p) {
    if (!p) return 0;
    if (p->pid == 1) return 1;
    process_t* cur = p;
    for (int depth = 0; depth < 16; depth++) {
        if (cur->pid == 1) return 1;
        if (cur->ppid <= 0 || cur->ppid == cur->pid) break;
        cur = process_find_by_pid(cur->ppid);
        if (!cur) break;
    }
    return 0;
}

int agent_bus_daemon_register(process_t* p) {
    if (!p) return -EINVAL;
    if (p->agent_id != 0) return -EPERM; /* must be non-agent */
    if (!is_init_tree_process(p)) return -EPERM; /* must be in init tree */

    kmutex_lock(&s_agent_lock);
    s_bus_daemon_pid = p->pid;
    kmutex_unlock(&s_agent_lock);

    kprintf("[bus] Registered PID %d as intent bus daemon\n", p->pid);
    return 0;
}

int agent_is_bus_daemon(process_t* p) {
    if (!p) return 0;
    return (s_bus_daemon_pid > 0 && p->pid == s_bus_daemon_pid);
}

int agent_model_daemon_register(process_t* p) {
    if (!p) return -EINVAL;
    if (p->agent_id != 0) return -EPERM; /* must be non-agent */
    if (!is_init_tree_process(p)) return -EPERM; /* must be in init tree */

    kmutex_lock(&s_agent_lock);
    s_model_daemon_pid = p->pid;
    kmutex_unlock(&s_agent_lock);

    kprintf("[model] Registered PID %d as model daemon\n", p->pid);
    return 0;
}

int agent_is_model_daemon(process_t* p) {
    if (!p) return 0;
    return (s_model_daemon_pid > 0 && p->pid == s_model_daemon_pid);
}

int agent_audit_append(int agent_id, uint32_t event_type, const char* detail) {
    process_t* me = scheduler_current();
    if (!agent_is_bus_daemon(me) && !agent_is_model_daemon(me)) return -EPERM;

    agent_audit_log(agent_id, event_type, detail);
    kprintf("[bus] AUDIT: agent %d event %u (%s): %s\n",
            agent_id, event_type, agent_event_name(event_type), detail ? detail : "");
    return 0;
}

int agent_charge_tokens(int agent_id, uint32_t tokens) {
    process_t* me = scheduler_current();
    if (!agent_is_bus_daemon(me) && !agent_is_model_daemon(me)) {
        return -EPERM;
    }

    if (agent_id <= 0) return 0; /* unconfined */

    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag) {
        kmutex_unlock(&s_agent_lock);
        return -ENOENT;
    }

    ag->usage.tokens_used += tokens;
    int exceeded = (ag->budget.max_tokens > 0 && ag->usage.tokens_used > ag->budget.max_tokens);
    uint32_t used = ag->usage.tokens_used;
    uint32_t max = ag->budget.max_tokens;
    kmutex_unlock(&s_agent_lock);

    if (exceeded) {
        char detail[64];
        detail[0] = '\0';
        int pos = 0;
        append_str_to_buf(detail, &pos, sizeof(detail), "token limit exceeded: ");
        append_uint_to_buf(detail, &pos, sizeof(detail), used);
        append_str_to_buf(detail, &pos, sizeof(detail), " > ");
        append_uint_to_buf(detail, &pos, sizeof(detail), max);
        agent_audit_log(agent_id, AUDIT_EVENT_BUDGET_EXCEEDED, detail);
        kprintf("[agent %d] BUDGET EXCEEDED: %s\n", agent_id, detail);
        return -EDQUOT;
    }

    return 0;
}

int agent_tokens_left(int agent_id) {
    if (agent_id <= 0) return 0x7FFFFFFF;

    kmutex_lock(&s_agent_lock);
    agent_t* ag = agent_get_by_id(agent_id);
    if (!ag) {
        kmutex_unlock(&s_agent_lock);
        return -ENOENT;
    }

    int left;
    if (ag->budget.max_tokens == 0) {
        left = 0x7FFFFFFF;
    } else if (ag->usage.tokens_used >= ag->budget.max_tokens) {
        left = 0;
    } else {
        left = (int)(ag->budget.max_tokens - ag->usage.tokens_used);
    }
    kmutex_unlock(&s_agent_lock);
    return left;
}
