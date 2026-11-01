# MyOS: Operating System Security Model & Threat Boundary

**Revision**: Phase 6.5 Closeout / Launch Readiness  
**Target Architecture**: x86-32 Protected Mode (Ring 0 / Ring 3)  
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

1. **Single-Core x86-32 Architecture**: MyOS runs on single-core 32-bit x86 protected mode. It does not utilize SMP or hardware hypervisor virtualization extensions (VT-x).
2. **No Hardware ASLR / NX Bit**: As a lightweight educational and demonstration OS, address space layout randomization (ASLR) and No-Execute (NX) page table protections are not implemented; spatial memory safety relies on C bounds checking and kernel heap canaries.
3. **Allowed-Domain Exfiltration**: If an agent is granted network access to an allowlisted external domain (e.g., `api.github.com`), malicious payloads can potentially exfiltrate data via standard HTTP query parameters or POST bodies within that domain.
4. **Bootstrapped Entropy in QEMU**: In virtualized environments lacking the `RDRAND` instruction, initial seed entropy relies on PIT timer jitter and RTC CMOS readings, which may possess lower cryptographic entropy during the initial 100ms of boot.
5. **Local Model Incompetence**: 15M parameter local language models are toy demonstrators. They cannot be trusted to follow JSON output schemas or obey complex instructions. Production agent autonomy requires the Host Bridge or direct API backend.
6. **FAT16 Filesystem Durability**: The underlying file storage uses FAT16 with a 64-entry LRU buffer cache. While consistent across clean shutdowns, sudden power loss during an active write may require `fsck.fat`.

