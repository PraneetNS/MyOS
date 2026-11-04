# MyOS: Operating System Security Model & Threat Boundary

**Revision**: Phase 8 Port & Modern CPU Hardening  
**Target Architecture**: x86-64 Long Mode (Ring 0 / Ring 3) with UEFI & BIOS Boot  
**Status**: Consolidated Architectural Specification & Threat Analysis

---

## 1. Executive Security Philosophy

MyOS is an AI-Native Operating System designed around a fundamental security principle: **autonomous agent containment cannot depend on model alignment, prompt filtering, or polite instruction following.**

In MyOS:
1. **Agents are Confined Principals**: Autonomous agents run in unprivileged userland (Ring 3) subject to strict kernel capabilities, execution budgets (tokens, network bytes, CPU ticks), and hardware-enforced isolation.
2. **Untrusted Data Origin**: All observation data flowing into an agent—whether from local files, user input, semantic memory, or network sockets—is tagged and treated as **hostile input** capable of containing indirect prompt injection.
3. **Defense in Depth**: Destructive actions (file system deletion), network exfiltration, and privilege escalation are stopped by independent, redundant architectural boundaries (Window Manager Approval Gate, Daemon Policy Engines, and Kernel Syscall Boundaries).
4. **Tamper-Evident Auditing**: All security-relevant actions, delegations, denials, and token consumptions are committed to a kernel-level, append-only, SHA-256 cryptographic hash chain.

---

## 2. Trust Boundaries & Isolation Hierarchy

```
+-------------------------------------------------------------------------+
|                  HOST SYSTEM (Developer / Hypervisor)                   |
|  - Cloud Provider Credentials (ANTHROPIC_API_KEY, OPENAI_API_KEY)       |
|  - Host Bridge Daemon (tools/hostbridge.py)                             |
|  - QEMU Emulator Boundary                                               |
+------------------------------------+------------------------------------+
                                     | COM2 Serial / VirtIO
=====================================v=====================================
+-------------------------------------------------------------------------+
|                       MYOS KERNEL (Ring 0)                              |
|  - Page Table Protections (Supervisor / User bit)                       |
|  - Capability Bitmask Enforcement (CAP_SYS_ADMIN, CAP_NET_RAW_INET, etc)|
|  - Hardware Network Killswitch (/proc/sys/net/killswitch)              |
|  - Timer & ISR Killswitch Toggle (Atomic flag, deferred work context)   |
|  - Resource Budgets (agent_charge_tokens, agent_charge_net)             |
|  - Cryptographic Audit Log (Append-only SHA-256 hash chain)             |
+-------------------+--------------------------------+--------------------+
                    |                                |
====================v================================v=====================
+-----------------------------------+  +----------------------------------+
|    SYSTEM DAEMONS (Ring 3, root)  |  |    UNCONFINED HUMAN USER (Ring 3)|
|  - init (/sbin/init)              |  |  - Interactive Shell (sh)        |
|  - Window Manager (wm)            |  |  - Agent Control CLI (agentctl)  |
|  - Intent Bus Daemon (intentd)    |  |  - Memory Control CLI (memctl)   |
|  - Model Daemon (modeld)          |  |  - Agent Panel GUI (agentpanel)  |
|  - Network Gateway (netd)         |  |                                  |
|  - Memory & Indexer (memd, indexd)|  |  * Can click Approval Dialogs    |
+-------------------+---------------+  |  * Can toggle Killswitch         |
                    | Intent Bus       +----------------------------------+
====================v======================================================
+-------------------------------------------------------------------------+
|                    SANDBOXED AGENTS (Ring 3, confined)                  |
|  - Principal-tagged process (/proc/agents/<id>/principal)               |
|  - Zero raw socket access (No CAP_NET_RAW_INET)                         |
|  - Bound by Intent Bus ACLs (CAP_BUS_CALL allowlist)                    |
|  - Window interaction restricted (CAP_WINDOW_ACT / CAP_WINDOW_OBSERVE)  |
|  - Blocked from clicking approval dialogs or toggling killswitch        |
+-------------------------------------------------------------------------+
```

---

## 3. Kernel Enforcement vs. Daemon Mediation

| Security Control | Enforcing Entity | Enforcement Mechanism | Failure / Bypass Consequence |
|:---|:---|:---|:---|
| **Raw Socket Deny** | **Kernel** (`src/socket.c`) | Process `is_agent` check on `socket(AF_INET, ...)`. Requires `CAP_NET_RAW_INET`. | Returns `-EACCES`, logs `AUDIT_EVENT_NET_DENIED`. Agent cannot bypass via userland. |
| **Token Budget Quota** | **Kernel** (`src/agent.c`) | `agent_charge_tokens()` atomic ledger against `budget.max_tokens`. | Returns `-EDQUOT`, logs `AUDIT_EVENT_BUDGET_EXCEEDED`. Halts agent generation. |
| **Bandwidth Quota** | **Kernel** (`src/agent.c`) | `agent_charge_net()` tracks byte ingress/egress. | Returns `-EDQUOT`, logs `AUDIT_EVENT_BUDGET_EXCEEDED`. Sockets fail to transmit. |
| **Killswitch Lockdown** | **Kernel** (`src/agent.c`, `virtio_net.c`) | PS/2 ISR sets atomic flag; timer tick & scheduler enforce immediate `SUSPENDED` state. | Agents blocked from execution within $\le 2$ PIT ticks. Hardware packets dropped. |
| **Domain Allowlist** | **Gateway Daemon** (`netd`) | Strict URL parsing, port matching, label boundary check, DNS rebinding re-resolution. | Blocks connection with HTTP `403 Forbidden` and logs `AUDIT_EVENT_NET_DENIED`. |
| **Human Approval Gate**| **Window Manager** (`wm`) | Server-side dialog intercepting destructive actions (`fs.delete`, privilege elevation). | Agent input dropped; agent cannot synthesize clicks on confirmation dialog. |
| **Memory Trust Labels**| **Memory Daemon** (`memd`) | Ignores client `from_tool` flags; automatically tags agent writes with `trust=agent`. | Agents cannot spoof `trust=user` to elevate prompt authority. |

---

## 4. Capability System & Principle of Least Privilege

Agents operate under a capability bitmask inherited at creation from their parent or explicitly granted by `agentctl create`:

1. **Windowing Capabilities**:
   - `CAP_WINDOW_OBSERVE:<app>`: Grants permission to query the Accessibility Tree (`ui.tree`) of matching window titles.
   - `CAP_WINDOW_ACT:<app>`: Permits synthesizing clicks and keystrokes on matching target windows.
   - **Preemption Safety**: Resumed agent actions re-verify window target ID, node ID, application allowlist, and node role before delivering input. If a user closes, minimizes, or replaces the window during an agent pause, the action cleanly aborts.
2. **Intent Bus Capabilities**:
   - `CAP_BUS_CALL:<prefix>`: Strict prefix match for callable skills (`fs.`, `model.`, `net.`, `mem.`). Any call outside the allowlist returns `-EACCES` over IPC.
3. **Memory Capabilities**:
   - `CAP_MEM_READ:priv`: Read records matching the agent's principal or shared records.
   - `CAP_MEM_WRITE:priv`: Write records tied to the agent's assigned principal.

---

## 5. Audit Log Guarantees and Limits

Every security-relevant decision is recorded to `/proc/agents/log`:
- **Cryptographic Chaining**: Each record computes $H_i = \text{SHA-256}(H_{i-1} \parallel \text{Timestamp} \parallel \text{AgentID} \parallel \text{Event} \parallel \text{Detail})$.
- **Zero Raw Secret Storage**: Prompts, completions, and HTTP request headers containing secrets are hashed or redacted (`[REDACTED_SECRET]`); raw sensitive tokens are **never** committed to disk or audit logs.
- **Audit Limits**: The audit log guarantees tamper detection (any mutation of past records invalidates all subsequent hashes), but does not guarantee non-repudiation if an attacker gains arbitrary physical kernel memory write access.

---

## 6. Known Architectural Limitations & Attack Surface

To maintain complete transparency, the following design constraints and known limitations exist in the current MyOS release:

1. **Single-Core x86-64 Architecture**: MyOS runs on single-core 64-bit x86-64 long mode. It does not yet utilize SMP (multi-core scheduling via APIC) or hardware hypervisor virtualization extensions (VT-x / AMD-V).
2. **CPU Hardening & ASLR Implementation**:
   - **Hardware Protections**: Hardware No-Execute (NX), SMEP (Supervisor Mode Execution Prevention, `CR4.SMEP=1`), SMAP (Supervisor Mode Access Prevention, `CR4.SMAP=1` with strict `stac`/`clac` uaccess bracketing) are enforced at boot.
   - **Userland ASLR Coverage**: Full 64-bit address space layout randomization is applied on process creation and `execve`:
     - **Stack Top**: 16 bits of entropy (8MB page slide + 5 bits of 16-byte sub-page alignment below `USER_STACK_TOP` `0x00007FFFFFFFE000`).
     - **Heap / Brk Base**: 17 bits of entropy (512MB slide above `HEAP_BASE` `0x0000000010000000`).
     - **Mmap Allocations**: 17 bits of entropy (512MB downward slide from `MMAP_TOP_START` `0x00007F0000000000`).
     - **PIE / ET_DYN Load Base**: 18 bits of entropy (1GB slide above standard base `0x0000555555550000`).
   - **KASLR Status**: Kernel Address Space Layout Randomization (KASLR) is **NOT** implemented; the kernel image remains deterministically mapped at `-2GB` (`0xFFFFFFFF80100000`). User stack-canary instrumentation remains optional compiler-assisted hardening.
3. **Allowed-Domain Exfiltration**: If an agent is granted network access to an allowlisted external domain (e.g., `api.github.com`), malicious payloads can potentially exfiltrate data via standard HTTP query parameters or POST bodies within that domain.
4. **Bootstrapped Entropy in QEMU**: In virtualized environments lacking the `RDRAND` instruction, initial seed entropy relies on PIT timer jitter, RTC CMOS readings, and saved `/var/lib/random-seed`.
5. **Local Model Incompetence**: 15M parameter local language models are toy demonstrators. They cannot be trusted to follow JSON output schemas or obey complex instructions. Production agent autonomy requires the Host Bridge or direct API backend.
6. **FAT16 Filesystem Durability**: The underlying file storage uses FAT16 with a 64-entry LRU buffer cache. While consistent across clean shutdowns, sudden power loss during an active write may require `fsck.fat`.

---

## 7. MyOS Syscall Security Policy (1000-Range Audit)

The MyOS kernel reserves the `1000+` syscall range for operating system extensions, agent containment, daemon registration, and hardware validation. Every custom syscall enforces strict boundary checks:

| Number | Syscall Name | Description | Caller Permission | Enforcement & Error on Violation |
|---|---|---|---|---|
| **1000** | `SYS_TICKS` | Query system timer ticks | Anyone | None (unrestricted read-only) |
| **1001** | `SYS_FREE_FRAMES` | Query free physical memory frames | Anyone | None (unrestricted read-only) |
| **1002** | `SYS_AGENT_CREATE` | Create child agent container | Unconfined root, or Agent with `CAP_SPAWN` and monotonic narrowing | `-EACCES` if agent lacks `CAP_SPAWN` or attempts privilege widening |
| **1003** | `SYS_AGENT_ATTACH` | Attach process to agent | Unconfined root only | `-EPERM` if called by sandboxed agent (`agent_id > 0`) |
| **1004** | `SYS_AGENT_KILL` | Terminate agent and all associated processes | Unconfined root only | `-EPERM` if called by sandboxed agent (`agent_id > 0`) |
| **1005** | `SYS_AGENT_SUSPEND` | Suspend running agent | Unconfined root only | `-EPERM` if called by sandboxed agent (`agent_id > 0`) |
| **1006** | `SYS_AGENT_RESUME` | Resume suspended agent | Unconfined root only | `-EPERM` if called by sandboxed agent (`agent_id > 0`) |
| **1007** | `SYS_AGENT_INFO` | Query agent metadata, limits, and usage | Unconfined root (any), Agent (self only) | `-EPERM` if sandboxed agent queries another agent |
| **1008** | `SYS_SOCK_PEERINFO` | Get peer PID, agent ID, and principal of domain socket | Anyone (caller must own socket fd) | `-EBADF` / `-ENOTSOCK` |
| **1009** | `SYS_BUS_DAEMON_REGISTER` | Register process as intent bus daemon | Unconfined root only (init process tree) | `-EPERM` if called by sandboxed agent or non-init process |
| **1010** | `SYS_AUDIT_APPEND` | Append cryptographic audit log record | Unconfined root (any), Agent (self only) | `-EPERM` if agent appends for another agent |
| **1011** | `SYS_AGENT_CHARGE_TOKENS` | Charge LLM token usage to agent | Intent bus daemon / Model daemon only | `-EPERM` if called by sandboxed agent or untrusted caller |
| **1012** | `SYS_AGENT_TOKENS_LEFT` | Query remaining LLM tokens for agent | Unconfined root (any), Agent (self only) | `-EPERM` if agent queries another agent |
| **1013** | `SYS_MODEL_DAEMON_REGISTER` | Register process as model daemon | Unconfined root only (init process tree) | `-EPERM` if called by sandboxed agent or non-init process |
| **1014** | `SYS_AGENT_KILLSWITCH_STATE` | Query or toggle global killswitch | Query: Anyone; Toggle: Unconfined root only | `-EPERM` if agent attempts toggle (`ebx == 1`) |
| **1015** | `SYS_AGENT_CHARGE_NET` | Charge network bandwidth usage to agent | Intent bus daemon / Net daemon only | `-EPERM` if called by sandboxed agent or untrusted caller |
| **1016** | `SYS_AGENT_NET_LEFT` | Query remaining network bytes | Unconfined root (any), Agent (self only) | `-EPERM` if agent queries another agent |
| **1017** | `SYS_NET_DAEMON_REGISTER` | Register process as network daemon | Unconfined root only (init process tree) | `-EPERM` if called by sandboxed agent or non-init process |
| **1018** | `SYS_TEST_HARDENING` | Debug SMEP/SMAP validation hook | Compiled out unless `MYOS_TESTING=1`; unconfined root only | `-ENOSYS` if compiled out; `-EPERM` if called by agent |
| **1020** | `SYS_TCGETATTR` | Get terminal attributes | Anyone (valid TTY fd required) | POSIX TTY permissions |
| **1021** | `SYS_TCSETATTR` | Set terminal attributes | Anyone (valid TTY fd required) | POSIX TTY permissions |
| **1099** | `SYS_TEST_KSTACK_OVERFLOW` | Debug kernel stack overflow recursion | Compiled out unless `MYOS_TESTING=1`; unconfined root only | `-ENOSYS` if compiled out; `-EPERM` if called by agent |

### Procfs & Process Visibility Sandbox Policy
- **Agent Self-Visibility Only**: Agents in `/proc/<pid>/` can inspect only their own PID or threads within their thread group. Any attempt to inspect `/proc/<other_pid>/` (including cmdline, environ, maps, status, fd) is denied with `-EACCES` or `-ENOENT`.
- **Prohibited Interfaces**: `/proc/<pid>/mem` is completely forbidden and not implemented in the kernel (always returning `-EACCES` or `-ENOENT`). Sandboxed agents cannot inspect or tamper with arbitrary process memory.

