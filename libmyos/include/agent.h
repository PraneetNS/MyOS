#ifndef _SYS_AGENT_H
#define _SYS_AGENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_AGENTS 32
#define MAX_PATH_RULES 8
#define MAX_PATH_PREFIX_LEN 64
#define AGENT_NAME_MAX 32

/* Capability bitmask flags */
#define CAP_FS_READ         (1 << 0)
#define CAP_FS_WRITE        (1 << 1)
#define CAP_EXEC            (1 << 2)
#define CAP_SOCKET_CONNECT  (1 << 3)
#define CAP_WINDOW          (1 << 4)
#define CAP_SPAWN           (1 << 5)
#define CAP_NET_PROXY       (1 << 6)
#define CAP_BUS_CALL        (1 << 7)
#define CAP_BUS_PROVIDE     (1 << 8)
#define CAP_WINDOW_OBSERVE  (1 << 9)
#define CAP_WINDOW_ACT      (1 << 10)
#define CAP_WINDOW_SECRET   (1 << 11)
#define CAP_WINDOW_RAW      (1 << 12)

/* Model backend capability flags */
#define MODEL_BACKEND_LOCAL  (1 << 0)
#define MODEL_BACKEND_HOST   (1 << 1)
#define MODEL_BACKEND_ALL    (MODEL_BACKEND_LOCAL | MODEL_BACKEND_HOST)

/* Agent states */
typedef enum {
    AGENT_STATE_UNUSED = 0,
    AGENT_STATE_RUNNING = 1,
    AGENT_STATE_SUSPENDED = 2,
    AGENT_STATE_KILLED = 3
} agent_state_t;

/* Audit log event types */
#define AUDIT_EVENT_SYSCALL_ALLOWED_SENSITIVE 1
#define AUDIT_EVENT_DENIED                    2
#define AUDIT_EVENT_BUDGET_EXCEEDED           3
#define AUDIT_EVENT_SPAWN                     4
#define AUDIT_EVENT_KILL                      5
#define AUDIT_EVENT_APPROVAL_REQUEST          6
#define AUDIT_EVENT_APPROVAL_GRANT            7
#define AUDIT_EVENT_APPROVAL_DENY             8
#define AUDIT_EVENT_BUS_REGISTER              9
#define AUDIT_EVENT_BUS_CALL                  10
#define AUDIT_EVENT_BUS_DENIED                11
#define AUDIT_EVENT_BUS_APPROVAL              12
#define AUDIT_EVENT_KILLSWITCH                13
#define AUDIT_EVENT_WINDOW_ACTION             14

typedef struct {
    char prefix[MAX_PATH_PREFIX_LEN];
} agent_path_rule_t;

typedef struct {
    uint32_t mask; /* CAP_* bits */
    uint32_t num_fs_read;
    agent_path_rule_t fs_read[MAX_PATH_RULES];
    uint32_t num_fs_write;
    agent_path_rule_t fs_write[MAX_PATH_RULES];
    uint32_t num_exec;
    agent_path_rule_t exec[MAX_PATH_RULES];
    uint32_t num_connect;
    agent_path_rule_t connect[MAX_PATH_RULES];
    uint32_t num_bus_call;
    agent_path_rule_t bus_call[MAX_PATH_RULES];
    uint32_t num_bus_provide;
    agent_path_rule_t bus_provide[MAX_PATH_RULES];
    uint32_t num_window_observe;
    agent_path_rule_t window_observe[MAX_PATH_RULES];
    uint32_t num_window_act;
    agent_path_rule_t window_act[MAX_PATH_RULES];
    uint32_t model_backends; /* Bitmask of allowed backends: 0 = MODEL_BACKEND_ALL */
} agent_caps_t;

typedef struct {
    uint32_t max_cpu_ticks;    /* 0 = unlimited */
    uint32_t max_mem_pages;    /* 0 = unlimited */
    uint32_t max_procs;        /* 0 = unlimited */
    uint32_t max_tokens;       /* 0 = unlimited */
} agent_budget_t;

typedef struct {
    uint32_t cpu_ticks;        /* Total CPU ticks consumed */
    uint32_t mem_pages;        /* Allocated memory pages */
    uint32_t procs_count;      /* Active processes in this agent */
    uint32_t tokens_used;      /* Counter charged by model daemon */
} agent_usage_t;

/* For SYS_AGENT_INFO query */
typedef struct {
    int id;
    char name[AGENT_NAME_MAX];
    int owner_pid;
    int parent_agent_id;
    agent_state_t state;
    agent_caps_t caps;
    agent_budget_t budget;
    agent_usage_t usage;
} agent_info_t;

/* Peer identity on connected AF_UNIX sockets */
typedef struct {
    int pid;
    int agent_id;
} peerinfo_t;

/* Audit log structure */
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t agent_id;
    uint32_t tick;
    uint32_t wall_time;
    uint32_t event_type;
    char detail[64];
} audit_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t prev_hash[32];
    audit_payload_t payload;
    uint8_t hash[32];
} audit_entry_t;

/* Userland syscall wrappers */
int agent_create(const char* name, const agent_caps_t* caps, const agent_budget_t* budget);
int agent_attach(int agent_id);
int agent_kill(int agent_id);
int agent_suspend(int agent_id);
int agent_resume(int agent_id);
int agent_info(int agent_id, agent_info_t* info);
int sock_peerinfo(int fd, peerinfo_t* pinfo);
int bus_daemon_register(void);
int model_daemon_register(void);
int audit_append(int agent_id, uint32_t event_type, const char* detail);
int agent_charge_tokens(int agent_id, uint32_t n);
int agent_tokens_left(int agent_id);
int agent_killswitch_state(void);
int agent_killswitch_toggle(void);

#ifdef __cplusplus
}
#endif

#endif
