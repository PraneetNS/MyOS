# Kernel Agents: AI-Native Operating System Subsystem

## 1. Executive Summary & Design Philosophy

As artificial intelligence models transition from external prompt-response APIs to autonomous operating system actors (autonomous code generation, tool invocation, automated workflow execution, background daemon tasks), treating them as ordinary unconfined user processes introduces catastrophic security, resource exhaustion, and auditability risks.

MyOS Phase 1 introduces **Agents as First-Class Kernel Objects**. Instead of relying on user-mode wrapper sandboxes or complex hypervisor overhead, the MyOS kernel directly tracks and enforces:
1. **Default-Deny Capabilities**: Fine-grained capability bitmasks coupled with canonical path-prefix allowlists enforced at the native VFS and system call layer.
2. **Strict Capability Narrowing**: Child agents spawned by parent agents can only inherit a strict subset of permissions—capabilities can be narrowed, but never widened.
3. **Rigid Resource Budgets**: Hard limits on CPU ticks, memory pages, active process count, and model inference tokens with automatic SIGXCPU-style suspension upon breach.
4. **Tamper-Evident Hash-Chained Audit Trail**: An in-kernel 64KB cryptographic ring buffer chaining all security events and sensitive operations with in-kernel SHA-256 digests (`SHA-256(prev_hash || payload)`).
5. **Seamless Process Binding & Inheritance**: Processes are bound to agents; `fork()` inherits the binding, `execve()` preserves it, and non-agent processes experience zero overhead or behavior alterations.

---

## 2. Architecture & Core Data Structures

### 2.1 The Agent Descriptor (`agent_t`)

Defined in `src/agent.h` and `libmyos/include/agent.h`:

```c
typedef struct agent {
    int id;                        /* Unique Agent ID (1..32) */
    char name[AGENT_NAME_MAX];     /* Human-readable label (e.g. "coder", "web_search") */
    char principal[AGENT_NAME_MAX];/* Stable persistent principal identity */
    int owner_pid;                 /* PID of creator / controller process */
    int parent_agent_id;           /* 0 if root agent, or parent agent ID if delegated */
    int in_use;                    /* Allocation flag */
    agent_state_t state;           /* RUNNING, SUSPENDED, KILLED, UNUSED */
    agent_caps_t caps;             /* Allowed capabilities and path prefixes */
    agent_budget_t budget;         /* Enforced resource limits (0 = unlimited) */
    agent_usage_t usage;           /* Monitored consumption counters */
} agent_t;
```

The kernel maintains a global table of up to 32 concurrent agent contexts (`s_agents[MAX_AGENTS]`), protected by a non-spinning sleeping mutex (`kmutex_t s_agent_lock`).

### 2.2 Process Binding & Lifecycle

Each kernel process (`process_t` in `src/process.h`) contains an `int agent_id;` field:
- `agent_id = 0`: Standard unconfined process (e.g., `/sbin/init`, interactive shell `sh`). Normal POSIX behavior is preserved without overhead.
- `agent_id > 0`: Process belongs to the specified agent. All sensitive system calls check the agent's capability mask and path allowlists.

**Lifecycle Invariants**:
- **Fork**: `process_fork()` verifies `agent_can_fork(p->agent_id)`. If within `max_procs`, the child inherits `child->agent_id = parent->agent_id` and the agent's `procs_count` counter increments. If the limit is reached, `fork()` fails with `-EAGAIN` and an audit event is logged.
- **Execve**: `execve()` preserves `me->agent_id`. In addition, before executing any binary, `agent_check_access(me, CAP_EXEC, path, "exec")` verifies execution rights against `caps.exec` path allowlists.
- **Exit / Teardown**: When any bound process terminates or exits (`process_destroy()`, `scheduler_exit_current()`), `agent_proc_exited(p->agent_id)` decrements the active process count.
- **Kill**: `agent_kill(agent_id)` marks the agent as `AGENT_STATE_KILLED` and sends `SIGKILL` to all active processes bound to that agent. The scheduler permits killed processes to run strictly to process signal termination and exit cleanly.

---

## 3. Capability Model (Default Deny)

When a process is attached to an agent, its security posture transitions to **default deny**. Unless a capability bit is set and the requested path prefix is explicitly listed in the allowlist, the syscall is denied with `-EACCES` and a serial log + audit trail event is emitted.

### 3.1 Capability Flags

| Capability | Bit | Scope |
|---|---|---|
| `CAP_FS_READ` | `1 << 0` | File opening with read access (`O_RDONLY`, `O_RDWR`). Must match `fs_read` prefix. |
| `CAP_FS_WRITE` | `1 << 1` | File writing, creation, truncation, deletion (`open(O_WRONLY|O_CREAT)`, `unlink`, `rename`, `mkdir`, `rmdir`). Must match `fs_write` prefix. |
| `CAP_EXEC` | `1 << 2` | Binary execution (`execve`). Must match `exec` prefix. |
| `CAP_SOCKET_CONNECT` | `1 << 3` | Socket connection (`connect`). Must match `connect` socket path prefix. |
| `CAP_WINDOW` | `1 << 4` | Permission to connect to the display server (`/run/display.sock`) and manage windows. |
| `CAP_SPAWN` | `1 << 5` | Permission to create or delegate child agents via `SYS_AGENT_CREATE`. |
| `CAP_NET_PROXY` | `1 << 6` | Permission to perform proxy operations across network sockets. |
| `CAP_BUS_CALL` | `1 << 7` | Permission to call Intent Bus skills matching skill prefix allowlist. |
| `CAP_BUS_PROVIDE` | `1 << 8` | Permission to register and provide Intent Bus skills matching prefix allowlist. |
| `CAP_WINDOW_OBSERVE` | `1 << 9` | Permission to inspect and read GUI accessibility trees of allowlisted apps. |
| `CAP_WINDOW_ACT` | `1 << 10` | Permission to dispatch structured accessibility actions to allowlisted apps. |
| `CAP_WINDOW_SECRET` | `1 << 11` | Permission to inspect/modify secret or password textboxes. Never inherited implicitly. |
| `CAP_WINDOW_RAW` | `1 << 12` | Permission to inject unmanaged raw keyboard/mouse events or frame screenshots. |
| `CAP_MEM_READ` | `1 << 13` | Permission to read memories from allowlisted namespaces (`priv`, `shared:<name>`). |
| `CAP_MEM_WRITE` | `1 << 14` | Permission to store or forget memories in allowlisted namespaces (`priv`, `shared:<name>`). |

### 3.2 Path Canonicalization & Prefix Matching

To prevent directory traversal attacks (e.g., `/home/agent/../../etc/shadow`), all paths are canonicalized prior to matching using `vfs_path_canonical(path, cwd, canon, sizeof(canon))`.
Prefix matching evaluates directory boundaries:
- Prefix `/home` permits `/home`, `/home/`, `/home/user`, `/home/user/file.txt`.
- Prefix `/home` does *not* permit `/homewrecker` or `/home_other`.

### 3.3 Strict Delegation Narrowing

When an existing agent invokes `agent_create()` to delegate tasks to a sub-agent:
1. The parent must possess `CAP_SPAWN`.
2. The child's capability mask must be a subset of the parent's mask: `(child_mask & ~parent_mask) == 0`.
3. Every path prefix granted to the child must match or be a subdirectory of an allowed path prefix in the parent:
   $$\forall p_{child} \in \text{Rules}_{child}, \exists p_{parent} \in \text{Rules}_{parent} \text{ s.t. } p_{child} \subseteq p_{parent}$$

Any attempt to widen privileges returns `-EACCES` and logs an audit security event.

---

## 4. Resource Budgets & Accounting

To prevent runaway loops, infinite recursions, or fork storms from disrupting the operating system, agents enforce four resource limits:

1. **`max_cpu_ticks`**:
   - PIT timer interrupt (`100Hz`) charges `agent_charge_cpu(cur->agent_id, 1)` on each active tick.
   - When cumulative ticks meet or exceed `max_cpu_ticks`, the agent is transitioned to `AGENT_STATE_SUSPENDED`.
   - The scheduler skips suspended processes during time-slice selection, keeping the desktop and user shell completely responsive.
2. **`max_mem_pages`**:
   - Tracks total allocated virtual and physical pages. Checked during allocation and page fault handling.
3. **`max_procs`**:
   - Limits concurrent running processes inside the agent. Fork attempts beyond this limit fail immediately with `-EAGAIN`.
4. **`max_tokens`**:
   - Monitored token consumption budget, queried and charged by AI model daemons.

---

## 5. Tamper-Evident Audit Log

The kernel implements a 64KB ring buffer of 256 audit log entries (`audit_entry_t`). Every security event is chained cryptographically using a self-contained in-kernel FIPS 180-2 SHA-256 implementation:

$$\text{hash}_n = \text{SHA-256}(\text{hash}_{n-1} \parallel \text{payload}_n)$$

Where `payload_n` is packed:
```c
typedef struct __attribute__((packed)) {
    uint32_t seq;           /* Monotonically increasing sequence number */
    uint32_t agent_id;      /* Agent ID */
    uint32_t tick;          /* Kernel tick timestamp */
    uint32_t wall_time;     /* UTC epoch seconds */
    uint32_t event_type;    /* ALLOWED, DENIED, BUDGET_EXCEEDED, SPAWN, KILL, etc. */
    char detail[64];        /* Contextual description */
} audit_payload_t;
```

The audit trail is exposed to userland via the virtual filesystem at `/proc/agents/log`. Because the previous block's SHA-256 hash is embedded directly in each entry, any post-facto modification or truncation is mathematically detectable by userland verifiers.

---

## 6. Threat Model

| Threat | Kernel Mitigation |
|---|---|
| **Unauthorized File Access** | Path canonicalization removes `..` and symlink indirection; VFS syscall checks enforce `CAP_FS_READ` / `CAP_FS_WRITE` prefix match before `open_file_alloc` or inode lookup. |
| **Data Exfiltration / Net Leak** | `CAP_SOCKET_CONNECT` restricts `connect()` calls to approved AF_UNIX paths; unapproved sockets return `-EACCES`. |
| **CPU Denial of Service (Spinners)** | PIT interrupt charges CPU ticks; threshold exceedance sets `AGENT_STATE_SUSPENDED`; process is descheduled without affecting system interactive responsiveness. |
| **Fork Bombs** | `agent_can_fork()` atomically tests `procs_count < max_procs` before PID allocation; returns `-EAGAIN` once budget is exhausted. |
| **Privilege Escalation via Child Spawning** | Delegation narrowing asserts bitmask subset and prefix inclusion; children can never gain wider access than parent. |
| **Audit Log Tampering** | Log buffer resides in protected kernel memory (`0xC0000000+`); ring buffer entries contain SHA-256 cryptographic chain. |

---

## 7. System Calls & Userspace API

### System Calls

- `int agent_create(const char* name, const agent_caps_t* caps, const agent_budget_t* budget)`: Syscall 170 (`SYS_AGENT_CREATE`). Creates a new agent.
- `int agent_attach(int agent_id)`: Syscall 171 (`SYS_AGENT_ATTACH`). Binds current process to agent.
- `int agent_kill(int agent_id)`: Syscall 172 (`SYS_AGENT_KILL`). Kills agent and sends SIGKILL to all bound processes.
- `int agent_suspend(int agent_id)`: Syscall 173 (`SYS_AGENT_SUSPEND`). Pauses execution of all agent processes.
- `int agent_resume(int agent_id)`: Syscall 174 (`SYS_AGENT_RESUME`). Resumes execution.
- `int agent_info(int agent_id, agent_info_t* info)`: Syscall 175 (`SYS_AGENT_INFO`). Queries agent metadata and metrics.

### ProcFS Interface

- `/proc/agents`: Directory listing active agent slots.
- `/proc/agents/log`: Read-only stream of all SHA-256 hash-chained audit events.
- `/proc/agents/<id>/status`: Agent state, process count, ticks, memory, and tokens.
- `/proc/agents/<id>/caps`: Bitmask and formatted list of path prefix allowlists.
- `/proc/agents/<id>/budget`: Configured maximum resource budgets.

### Management Tool (`agentctl`)

```bash
# Create an agent restricted to reading /home and executing /bin
agentctl create myagent --fs-read /home --exec /bin --cpu 500 --mem 64

# List all active agents and their resource usage
agentctl list

# Inspect the live cryptographic audit log
agentctl log -f

# Run a command inside an agent
agentctl run 1 cat /home/hello.txt

# Terminate an agent and all its child processes
agentctl kill 1
```
