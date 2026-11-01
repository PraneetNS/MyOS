# netd - Agent Network Gateway Daemon

`netd` is the privileged, sandboxed network gateway daemon in MyOS. It connects to the Intent Bus (`/run/intent.sock`) and exposes controlled network capabilities to sandboxed agents while enforcing strict domain allowlists, SSRF protections, secret injection, DNS rebinding mitigation, and BearSSL-based TLS verification.

---

## 1. Registered Skills

`netd` registers four primary skills on the MyOS Intent Bus:

### 1.1 `net.http`
Executes an HTTP or HTTPS request on behalf of an agent.
- **Parameters**:
  - `method` (string): HTTP method (`GET`, `POST`, `PUT`, `DELETE`).
  - `url` (string): Full destination URL (e.g. `https://api.example.com/v1/data`).
  - `headers` (object, optional): Custom headers supplied by the caller.
  - `body` (string, optional): Request body payload.
  - `timeout_ms` (integer, optional): Request timeout in milliseconds (capped at 15,000ms).
- **Return Value**:
  - `status` (integer): HTTP response status code (e.g. 200, 404).
  - `headers` (object): Normalized response headers.
  - `body` (string): Response body payload (truncated to 64 KB if larger).
  - `truncated` (boolean): `true` if response exceeded size ceiling.
  - `untrusted` (boolean): Always `true` for all external responses.

### 1.2 `net.resolve`
Resolves domain names to IPv4 addresses.
- **Parameters**:
  - `host` (string): Hostname to resolve.
- **Return Value**:
  - `ip` (string): Resolved IPv4 address.
  - `ttl` (integer): Cache TTL in seconds.

### 1.3 `net.status`
Queries interface status and link metrics.
- **Return Value**:
  - `interfaces` (array): List of active network interfaces, IP addresses, MAC addresses, and packet counters.
  - `killswitch` (boolean): Current state of the network killswitch.

*(Note: `net.killswitch` was removed from the Intent Bus in Phase 6.5 to eliminate bus-based escalation; the network killswitch is controlled directly via `/proc/sys/net/killswitch`, which the kernel strictly denies to agents with `-EPERM`).*

---

## 2. Security Enforcements

### 2.1 Strict URL Parser
`netd` parses all incoming URLs with an RFC-compliant strict parser:
- Rejects userinfo components (`user:pass@host`).
- Rejects backslashes (`\`) and control characters.
- Rejects percent-encoded host tricks and mixed-case obfuscation.
- Normalizes trailing dots (`example.com.` -> `example.com`) and paths.
- Rejects IP literals (`http://10.0.2.2/`) unless the agent principal is explicitly configured with `allow_private=true`.

### 2.2 Domain Label Boundary Matching
Domain allowlists strictly enforce DNS label boundaries:
- A pattern `*.example.com` matches `api.example.com` or `dev.sub.example.com`.
- It **never** matches `evil-example.com` or `notexample.com`.

### 2.3 SSRF Prevention & DNS Rebinding Mitigation
To prevent agents from weaponizing `net.http` against local services:
- **Private Subnets Blocked**: All private, link-local, carrier-grade NAT, multicast, and broadcast subnets (`127.0.0.0/8`, `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `169.254.0.0/16`, `100.64.0.0/10`, `224.0.0.0/4`, `255.255.255.255/32`, `0.0.0.0/8`) are rejected with HTTP 403 unless explicitly allowed for the principal.
- **Pinning**: DNS resolution pins the destination IP address at connection time; HTTP redirects are re-resolved and re-validated against the blocklist on every hop (up to 5 hops maximum).
- **No Protocol Downgrades**: HTTPS to HTTP redirect downgrades are strictly forbidden.

### 2.4 Per-Agent Concurrency Limits & Quotas
- **Concurrency**: Agents are capped at a maximum of 4 active in-flight requests simultaneously.
- **Bandwidth Quota**: Traffic (incoming headers + body, outgoing headers + payload) is charged to the agent's `max_net_bytes` budget via `agent_charge_net()`. Breaches trigger `-EDQUOT` and log `AUDIT_EVENT_BUDGET_EXCEEDED`.

### 2.5 Secret Injection
Agents must never hold sensitive API tokens directly in memory or prompts. Instead:
- Configuration files in `/etc/net-secrets.d/<principal>.conf` define secret mappings per principal:
  ```ini
  domain=api.example.com
  header=Authorization: Bearer secret-token-key-abc
  ```
- When an agent with matching principal calls `net.http` for that domain, `netd` automatically injects the secret header before transmission.
- Secret headers are scrubbed from log files and responses returned to the agent.

### 2.4 BearSSL TLS 1.2 Handshake
When an agent connects to `https://`:
- `netd` utilizes the in-tree `libtls` BearSSL wrapper.
- Root CA anchors are loaded from `/etc/ssl/anchors.pem`.
- Server certificates, validity periods, and SANs are verified before transmitting plaintext data.

