# Intent Bus: Secure Inter-Agent Skill Invocation

## 1. Executive Summary & Design Goals

In an AI-native operating system, intelligent agents do not operate in isolation: they expose specialized tools ("skills") and collaborate by invoking skills provided by other agents. However, allowing arbitrary inter-process communication between untrusted or semi-autonomous agents introduces critical attack surfaces:
1. **Identity Spoofing**: An unprivileged agent impersonating an administrative or elevated agent to invoke privileged actions.
2. **Confused Deputy Attacks**: A low-privilege caller tricking a high-privilege provider into accessing resources on its behalf.
3. **Bus Starvation / DoS**: Malicious agents flooding the message broker, creating circular invocation deadlocks, or hanging provider connections indefinitely.
4. **Approval Bypass**: Executing sensitive actions (e.g., file deletion, external network modifications, financial or administrative operations) without user/administrator consent.

MyOS Phase 2 addresses these challenges by implementing the **Intent Bus**: a kernel-authenticated, capability-enforced, single-threaded bus daemon (`intentd`) communicating over local Unix domain sockets (`/run/intent.sock`) with MCP-inspired JSON protocol framing.

---

## 2. Architecture & Components

```
                +-----------------------------------------+
                |         intentd (/run/intent.sock)      |
                |  - Single-threaded poll() loop          |
                |  - Kernel peer authentication           |
                |  - Capability allowlist checking        |
                |  - Schema validation (libjson)          |
                |  - Sensitive approval queue             |
                |  - Cryptographic audit trail append     |
                +-------------------+---------------------+
                                    |
            +-----------------------+-----------------------+
            |                                               |
+-----------v-----------+                       +-----------v-----------+
|     Caller Agent      |                       |    Provider Agent     |
|   (Agent ID = 2)      |                       |    (Agent ID = 3)     |
|  CAP_BUS_CALL("fs.")  |                       | CAP_BUS_PROVIDE("fs.")|
|                       |                       | CAP_FS_READ("/home")  |
|  intent_call(         |                       |                       |
|   "fs.read", args)    |                       | intent_register_skill(|
|                       |                       |  "fs.read", schema)   |
+-----------------------+                       +-----------------------+
```

### 2.1 The Bus Daemon (`intentd`)
- Launched as a `respawn` service from `/etc/init.conf`.
- Registers itself as the privileged bus daemon via `SYS_BUS_DAEMON_REGISTER`. Only non-agent processes residing in the init process tree can register.
- Appends cryptographic audit records directly into the kernel SHA-256 audit log via `SYS_AUDIT_APPEND`.
- Listens on `/run/intent.sock` using an event-driven `poll()` loop with bounded memory buffers.

### 2.2 Kernel Peer Identity (`SYS_SOCK_PEERINFO`)
When an AF_UNIX connection is established (`connect()` / `accept()`), the kernel captures the exact `{pid, agent_id}` of both endpoints in the socket descriptor (`socket_t`). The syscall `SYS_SOCK_PEERINFO(fd, &peerinfo)` retrieves this immutable identity. The daemon **never** trusts or parses caller identity from message payloads.

### 2.3 Kernel Capabilities: `CAP_BUS_CALL` and `CAP_BUS_PROVIDE`
- **`CAP_BUS_CALL(prefix)`**: Controls which skill name prefixes (e.g., `"fs."`, `"sys."`) an agent is permitted to invoke.
- **`CAP_BUS_PROVIDE(prefix)`**: Controls which skill name prefixes an agent is permitted to register and provide.
- Both capabilities are stored in kernel `agent_t->caps`, obey strict delegation narrowing rules, and are reflected in `/proc/agents/<id>/caps`.

---

## 3. Protocol Specification

### 3.1 Wire Framing Header (12 Bytes)

All socket messages begin with a 12-byte header matching the `win_protocol.h` format:

```c
struct intent_header {
    uint32_t len;      /* Total message size including this header (max 16384) */
    uint16_t type;     /* INTENT_MSG_* message type identifier */
    uint16_t flags;    /* Reserved flags / protocol version */
    uint32_t call_id;  /* Correlation ID matching request and response */
};
```

Immediately following the 12-byte header is the JSON body encoded in UTF-8.

### 3.2 Message Types

| Type ID | Constant | Direction | Payload Schema | Description |
|---|---|---|---|---|
| `1` | `INTENT_MSG_REGISTER` | Client -> Daemon | `{"name": str, "description": str, "input_schema": obj, "sensitive": bool}` | Register a new skill |
| `2` | `INTENT_MSG_REGISTER_ACK`| Daemon -> Client | `{"ok": bool, "error": str}` | Registration result |
| `3` | `INTENT_MSG_UNREGISTER` | Client -> Daemon | `{"name": str}` | Unregister a skill |
| `4` | `INTENT_MSG_UNREGISTER_ACK`| Daemon -> Client | `{"ok": bool, "error": str}` | Unregistration result |
| `5` | `INTENT_MSG_LIST` | Client -> Daemon | `{}` | List registered skills |
| `6` | `INTENT_MSG_LIST_ACK` | Daemon -> Client | `{"skills": [{"name": str, "description": str, "provider_agent": int, "sensitive": bool}]}` | Active skills listing |
| `7` | `INTENT_MSG_CALL` | Caller -> Daemon | `{"skill": str, "args": obj}` | Invoke a skill |
| `8` | `INTENT_MSG_CALL_FORWARD`| Daemon -> Provider| `{"call_id": int, "skill": str, "args": obj, "caller_agent": int, "caller_pid": int}` | Forward call to provider |
| `9` | `INTENT_MSG_RESULT` | Both directions | `{"call_id": int, "ok": bool, "result": any, "error": str}` | Return execution result |
| `10` | `INTENT_MSG_LIST_APPROVALS`| Client -> Daemon | `{}` | Query pending approvals |
| `11` | `INTENT_MSG_LIST_APPROVALS_ACK`| Daemon -> Client| `{"approvals": [{"id": int, "skill": str, "caller_agent": int, "provider_agent": int}]}` | List pending approvals |
| `12` | `INTENT_MSG_RESOLVE_APPROVAL`| Client -> Daemon| `{"id": int, "approved": bool}` | Approve or deny pending call |
| `13` | `INTENT_MSG_RESOLVE_APPROVAL_ACK`| Daemon -> Client| `{"ok": bool, "error": str}` | Approval resolution ack |
| `14` | `INTENT_MSG_STATS` | Client -> Daemon | `{}` | Request daemon statistics |
| `15` | `INTENT_MSG_STATS_ACK` | Daemon -> Client | `{"calls_total": int, "calls_succeeded": int, "calls_denied": int, ...}` | Bus metrics |

---

## 4. Threat Model & Security Mitigations

### 4.1 Spoofing Defense
- **Threat**: An agent crafts a JSON payload containing `"caller_agent": 0` or `"caller_agent": 1` to impersonate root or another agent.
- **Mitigation**: The daemon calls `sock_peerinfo(client_fd, &pinfo)` on the connected socket. The kernel returns the authentic `agent_id` and `pid` assigned during `agent_create()` or inherited on `fork()`. Any identity fields present in the message body are completely ignored.

### 4.2 Confused Deputy Defense
- **Threat**: A low-privilege caller with read access limited to `/tmp` invokes `fs.read` provided by an agent that has wider filesystem access, attempting to exfiltrate `/etc/init.conf` or sensitive keys.
- **Mitigation**:
  1. **Bus Layer**: The caller must have `CAP_BUS_CALL("fs.")`.
  2. **Provider Sandbox Layer**: When the provider attempts to open `/etc/init.conf`, the **kernel** VFS capability sandbox intercepts the syscall (`SYS_OPEN`). Because the provider's `agent_t` only holds `CAP_FS_READ("/home")`, the kernel immediately denies the syscall with `-EACCES`.
  3. **Dual Audit Logging**: Both the bus invocation and the underlying kernel capability violation are recorded in `/proc/agents/log`. The kernel remains the non-bypassable last line of defense.

### 4.3 Denial of Service (DoS) & Bus Starvation
- **Threat**: A rogue agent floods the bus with millions of requests, opens thousands of connections, or registers skills with recursive schemas.
- **Mitigation**:
  1. **Rate Limiting**: Per-caller sliding window rate limit of `50 calls/second`. Excess calls receive an immediate `Rate limit exceeded` denial and are logged under `BUS_DENIED`.
  2. **In-Flight Concurrency Cap**: Maximum `8` concurrent in-flight calls per caller.
  3. **Bounded Timeouts**: All provider calls default to a hard `5-second timeout`. If a provider hangs, the daemon responds with `Provider call timed out` and cleans up in-flight slots.
  4. **Provider Crash Cleanup**: If a provider crashes or closes its socket mid-call, the daemon catches `POLLHUP`/`POLLERR`, immediately aborts in-flight calls with clean errors to callers, and unregisters the dead provider's skills.
  5. **Bounded Parser**: `libjson` enforces strict maximum nesting depth of 16 and bounded node pools to prevent stack exhaustion.

### 4.4 Approval Bypass & Sensitive Skills
- **Threat**: An autonomous agent invokes destructive skills (e.g. `fs.delete`, `sys.reboot`) without human-in-the-loop oversight.
- **Mitigation**:
  1. Skills registered with `sensitive: true` do not forward calls to providers immediately.
  2. Instead, `intentd` generates a pending approval entry, emits `BUS_APPROVAL (pending)` into the audit log, and holds the caller's connection.
  3. The administrator approves or denies the call using `intentctl approve <id>` or `intentctl deny <id>`.
  4. If unresolved after `30 seconds`, the daemon enforces a **default deny** policy (`Approval timed out (auto-denied)`).

### 4.5 Audit Logging with Privacy Preservation
- **Threat**: Audit logs could expose sensitive passwords, secret API keys, or private document contents passed as arguments.
- **Mitigation**: The daemon computes a SHA-256 digest of the arguments string (`SHA-256(args)`) and records only the first 16 hexadecimal characters of the hash in the audit event detail:
  ```
  c:2 p:3 fs.read rc:0 h:a1b2c3d4e5f60718
  ```
  Full audit integrity is preserved in the cryptographic hash chain without leaking argument contents.

---

## 5. Client Libraries & Tooling

### 5.1 `libjson` (`userland/libjson/`)
Dependency-free, zero-malloc-in-hot-path JSON parser and serializer:
- Bounded token capacity (`JSON_MAX_NODES = 512`) and depth limit (`JSON_MAX_DEPTH = 16`).
- Native schema validation (`json_validate_schema`) validating JSON Schema types and `required` property constraints.

### 5.2 `libintent` (`userland/libintent/`)
Lightweight C client library providing:
- `intent_connect()` and `intent_close(fd)`.
- `intent_register_skill(fd, name, desc, in_schema, out_schema, sensitive, handler, udata)`.
- `intent_serve(fd)` single-threaded dispatch loop.
- `intent_call(fd, skill, json_args, json_result, cap, timeout_ms)`.
- `intent_list_skills()`, `intent_list_approvals()`, `intent_resolve_approval()`, `intent_get_stats()`.

### 5.3 Lua Language Bindings (`userland/lua/src/lua_intent.c`)
Enables writing and consuming skills directly in Lua:
```lua
local intent = require("intent")
intent.register("math.add", {type="object", required={"a", "b"}}, function(args)
    return { sum = args.a + args.b }
end)
intent.serve()
```

### 5.4 Management CLI: `intentctl`
```bash
# List all registered skills, their providers, and sensitivity status
intentctl list

# Call a skill from the shell
intentctl call fs.read '{"path":"/home/readme.txt"}'

# Inspect pending approvals
intentctl approvals

# Approve or deny a sensitive invocation
intentctl approve 1
intentctl deny 2

# Inspect bus metrics and statistics
intentctl stats
```
