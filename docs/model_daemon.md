# Model Daemon: Host Bridge, Token Budgets, Local Inference & Autonomous Agent Loop

## 1. Executive Summary & Design Goals

In an AI-Native Operating System, AI models must be available to agents as standard, governed system facilities rather than external, unmanaged ad-hoc network calls. Introducing LLM generation directly into the OS presents unique architectural challenges:
1. **Host-Guest Trust Boundary**: Cloud LLM API keys (Anthropic, OpenAI) must **never** enter guest userland or disk. A compromised guest agent or buffer overflow must never expose provider credentials.
2. **Resource Exhaustion & Financial Denial of Service**: Unchecked autonomous agents running infinite reasoning loops can drain token allocations or incur massive API costs. Token consumption must be budgeted by the kernel, clamped before generation, and strictly charged upon completion.
3. **Hardware & Execution Heterogeneity**: The OS must support both local inference (when disconnected, private, or cost-constrained) and host-bridged model inference (for complex frontier model reasoning), governed by per-agent policy allowlists.
4. **Prompt Injection & Adversarial Payloads**: Autonomous agents consume tool observation data from untrusted sources (files, network, user input). Malicious payloads must be explicitly labeled and quarantined to prevent hijacking agent execution.
5. **Auditing & Privacy**: Inter-agent prompts and completions often contain sensitive personal data. The kernel audit chain must verify invocation provenance without storing raw prompts or completions on disk.

MyOS Phase 3 resolves these challenges through five core layers:
- **Raw COM2 Serial Device (`/dev/serial1`)**: High-performance, raw, non-TTY byte device (port `0x2F8`, IRQ3) with ring-buffered RX, `poll()` support, and `O_NONBLOCK`.
- **Host Bridge (`tools/hostbridge.py`)**: Python-based daemon running on the host connecting to QEMU's second serial port. API keys are kept strictly in host memory/environment; responses are length-prefixed and CRC32-framed.
- **Local Model Engine (`userland/llm/libllm.a`)**: llama2.c-inspired transformer inference engine in pure C with INT8 quantization, greedy and temperature sampling, and seedable PRNG.
- **Model Daemon (`userland/modeld.c`)**: Intent Bus service provider exposing `model.complete`, `model.embed`, and `model.status`, enforcing token budgets, kernel caps, and prompt SHA-256 audit logging.
- **Agent Loop (`agentrun` & Lua `agent` module)**: Autonomous agent orchestrator fetching tools from the Intent Bus, running multi-step reasoning loops with prompt injection defense.

---

## 2. Architecture & Trust Model

```
       HOST SYSTEM (Untrusted to Guest / Trusted for Secrets)
   +--------------------------------------------------------------+
   | tools/hostbridge.py (Python stdlib)                          |
   |   - Backends: mock, anthropic, openai_compat                 |
   |   - API Keys: ANTHROPIC_API_KEY, OPENAI_API_KEY (Host env)   |
   |   - Framing: [Length: uint32][CRC32: uint32][JSON Payload]   |
   +-------------------------------+------------------------------+
                                   | TCP / Socket Chardev
                                   | (QEMU -serial tcp:127.0.0.1:4445)
===================================v===================================
   GUEST KERNEL (MyOS Protected Mode)
   +--------------------------------------------------------------+
   | /dev/serial1 (COM2 0x2F8, IRQ3, raw byte ring buffer, poll)  |
   | Token Budgets: SYS_AGENT_CHARGE_TOKENS, SYS_AGENT_TOKENS_LEFT|
   | Security: MODEL_BACKEND_LOCAL | MODEL_BACKEND_HOST caps      |
   | Audit Log: SHA-256 Hash Chain with prompt hex hashes         |
   +-------------------------------+------------------------------+
                                   |
   GUEST USERLAND                  |
   +-------------------------------v------------------------------+
   | modeld (Intent Bus Provider at /run/intent.sock)             |
   |   - Skills: model.complete, model.embed, model.status        |
   |   - Enforces remaining token budget before dispatch          |
   |   - Charges actual usage via kernel syscall after completion |
   |   - Dispatches to Host Bridge or Local libllm                |
   +-------------------------------+------------------------------+
                                   |
            +----------------------+----------------------+
            | Intent Bus                                  | Intent Bus
+-----------v-----------+                     +-----------v-----------+
| agentrun (CLI Loop)   |                     | Lua Agent Module      |
|  - Goal-directed loop |                     |  - require("agent")   |
|  - Tool discovery     |                     |  - agent.run()        |
|  - Injection defense  |                     |  - Scriptable agents  |
+-----------------------+                     +-----------------------+
```

### 2.1 The Host-Guest Trust Boundary
- **Zero Key Leakage**: API keys never cross the serial port into the guest VM. The guest only receives structured model responses (`text`, `tool_calls`, `usage`).
- **Resilient Framing**: The wire protocol uses `[Length: 4B Big-Endian][CRC32: 4B Big-Endian][JSON Body]`. If line noise or garbage bytes occur, the forward scanner automatically resynchronizes to the next valid frame.
- **Clamping**: Host-side maximum token generation is clamped to prevent rogue runaway queries.

---

## 3. Kernel Subsystems

### 3.1 Raw Second Serial Port (`/dev/serial1`)
- **Port**: COM2 (`0x2F8`, IRQ3, interrupt vector 35).
- **Driver**: Implemented in `src/serial.c` with dedicated `s_serial1_rx_buf` (8KB circular buffer), `s_serial1_read_wait` wait channel, and `serial1_poll()`.
- **Isolation**: Completely decoupled from COM1 (`serial0`), which serves as the interactive text console.

### 3.2 Token Budgets & System Calls
- **`SYS_AGENT_CHARGE_TOKENS` (Syscall 179)**:
  `int agent_charge_tokens(int agent_id, uint32_t n)`
  Only callable by a registered model daemon process. Charges `n` tokens against the agent's budget. If `usage.tokens + n > budget.max_tokens`, charges up to the limit, returns `-EDQUOT` (132), and logs `AUDIT_EVENT_BUDGET_EXCEEDED` into the kernel audit log.
- **`SYS_AGENT_TOKENS_LEFT` (Syscall 180)**:
  `int agent_tokens_left(int agent_id)`
  Returns the remaining tokens available before hitting `budget.max_tokens`.
- **`SYS_MODEL_DAEMON_REGISTER` (Syscall 181)**:
  `int model_daemon_register(void)`
  Restricted to processes in the PID-tree of init without an active agent attachment. Enables token charging privileges.

### 3.3 Backend Delegation Allowlist
- `agent_caps_t.model_backends`: Bitmask containing `MODEL_BACKEND_LOCAL` (1) and `MODEL_BACKEND_HOST` (2).
- Enforces strict delegation narrowing: a child agent cannot have backends not possessed by its parent.
- Configurable via `agentctl create --model-backend local,host` and inspected in `/proc/agents/<id>/caps`.

---

## 4. Model Daemon (`modeld`)

`modeld` runs as a persistent service registered in `/etc/init.conf`. It provides three Intent Bus skills:
1. **`model.complete`**:
   - Accepts `messages` array, optional `tools` definitions, `max_tokens`, `temperature`, and `backend` (`auto`, `local`, `host`).
   - Checks `SYS_AGENT_TOKENS_LEFT` and clamps `max_tokens` to remaining budget.
   - Hashes prompt bytes using SHA-256 and records the prompt hash into the kernel audit log (`detail="model c:<id> b:<backend> in:<in> out:<out> h:<hash>"`). Raw prompt/completion text is **never** written to audit logs.
   - Charges consumed tokens via `SYS_AGENT_CHARGE_TOKENS`.
2. **`model.embed`**:
   - Generates normalized 64-dimensional feature vector embeddings from input text via locality-sensitive bag-of-words hashing.
3. **`model.status`**:
   - Reports availability of local model and host bridge connectivity.

---

## 5. Local Inference Engine (`libllm`) & Performance

`userland/llm/` provides a pure C transformer inference engine capable of loading INT8 quantized checkpoints (`runq` format) and FP32 weights:
- **Memory Footprint**: Memory-mapped or sbrk-allocated; the default `stories-260k` model occupies < 400KB on disk and runs smoothly even under constrained `-m 64` memory environments.
- **Performance under QEMU TCG**:
  - Tiny checkpoint (`stories-260k`, 260K params): **~35 tokens/sec** under pure QEMU i386 TCG software emulation.
  - Mock host bridge (`tools/hostbridge.py`): **~140 tokens/sec** round-trip over COM2 serial channel.
  - Production host bridge (Anthropic/OpenAI): constrained only by remote API latency and host network speed.

---

## 6. Autonomous Agent Loop (`agentrun`)

`agentrun` executes an autonomous ReAct-style agent loop:
1. **Tool Discovery**: Queries `skill.list` from the Intent Bus, filtering skills against the agent's `CAP_BUS_CALL` allowlist.
2. **Reasoning Step**: Sends conversation history to `model.complete`.
3. **Prompt Injection Defense**: Tool results are sanitized, truncated, and strictly wrapped with:
   ```
   [UNTRUSTED DATA FROM TOOL <skill>]
   <observation>
   [END UNTRUSTED DATA]
   ```
   The agent's system prompt explicitly instructs the model never to treat text inside untrusted blocks as system commands.
4. **Execution Trace**: Real-time thought/action/observation steps are displayed on console and persisted to `/home/out/agent-<id>.log`.

