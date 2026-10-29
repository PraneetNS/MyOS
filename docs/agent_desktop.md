# The Agent Desktop: Autonomous GUI Operation, Structured Accessibility, and Trust Architecture

## 1. Executive Summary & Design Principles

In traditional graphical environments, autonomous agents interact with user interfaces by streaming raw framebuffers (screenshots) to multimodal vision-language models and synthesizing mouse clicks and keyboard scancodes. While flexible, this approach suffers from severe limitations:
1. **High Token & Computational Cost**: Transmitting and processing megabytes of image bitmaps per second overwhelms host-guest bandwidth and drains token budgets.
2. **Imprecision & Brittleness**: Pixel-based coordinate guessing breaks across resolutions, font scales, themes, and dynamic layout reflows.
3. **Security Vulnerabilities**: Vision-based screen scraping exposes sensitive application data (e.g., password inputs, private keys) and lacks fine-grained access control.
4. **Lack of Human Oversight & Safety**: Autonomous agents operating in background threads can conflict with human input or execute destructive actions without observable attribution or instant override mechanisms.

**MyOS Phase 4: The Agent Desktop** resolves these limitations by introducing a first-class, capability-secured accessibility tree architecture directly integrated into the kernel, window manager, and widget toolkit. Rather than scraping pixels, agents observe semantic widget hierarchies and dispatch structured actions directly to application widget event handlers under strict kernel-enforced sandboxes.

```
+-------------------------------------------------------------------------------+
|                             Compositor (wm.c)                                 |
|  - Authenticates display clients via SYS_SOCK_PEERINFO                        |
|  - Enforces CAP_WINDOW_OBSERVE / CAP_WINDOW_ACT with per-app allowlists       |
|  - Renders visible control indicators (colored borders, title badges)         |
|  - Composites trusted modal approval dialogs (/run/approval.sock)             |
|  - Displays emergency stop banner on killswitch activation                   |
|  - Human-wins collision detection (hardware input aborts agent actions)       |
+---------------------------------------+---------------------------------------+
                                        | Privileged Window Protocol
                                        v
+-------------------------------------------------------------------------------+
|                       UI Bridge Daemon (uibridge.c)                           |
|  - Privileged bus provider connecting wm display server and Intent Bus        |
|  - Formats accessibility trees into LLM-optimized compact indented text       |
|  - Exposes skills: ui.list_windows, ui.get_tree, ui.act, ui.launch,           |
|                    ui.wait, ui.focus_window                                   |
|  - Spawns requested apps inside caller's agent sandbox (agent_attach)         |
+---------------------------------------+---------------------------------------+
                                        | Intent Bus (/run/intent.sock)
                                        v
+-------------------------------------------------------------------------------+
|                   Autonomous Agents & Dashboard (agentpanel)                  |
|  - Agent Dashboard (agentpanel): live monitoring, preset launch, Ask bar      |
|  - Task Agents: organizer, explainer, terminal-helper                         |
|  - Kernel Killswitch (Ctrl+Alt+Esc) suspending all agents at interrupt level  |
+-------------------------------------------------------------------------------+
```

---

## 2. Structured Accessibility Tree Engine (`libui` & `libwin`)

All GUI applications built with `libui` automatically expose a semantic accessibility hierarchy without requiring any per-application modifications.

### 2.1 Node Hierarchy & Roles
Every widget registered with `ui_window` is assigned a unique, monotonically increasing node identifier (`uint32_t node_id`) that remains stable across layout passes. Each node exports:
- **Role**: `window`, `button`, `checkbox`, `radio`, `textbox`, `textarea`, `label`, `listbox`, `listitem`, `tab`, `menu`, `menuitem`, `scrollbar`, `progress`, `dialog`, or `terminal`.
- **Name / Label**: Human-readable widget text or accessibility name.
- **Value**: Current textual or state value (e.g., textbox text, checkbox state, slider percentage).
- **Bounds**: Global screen coordinates and dimensions `(x, y, w, h)`.
- **States**: Bitmask of flags (`focused`, `enabled`, `checked`, `selected`, `hidden`).
- **Children**: Ordered array of child node IDs.

### 2.2 Terminal Accessibility (`term.c`)
The graphical terminal emulator (`term.c`) exposes an accessibility node with role `terminal`. Its value contains the currently visible terminal text lines formatted with newline separators. It supports the structured action `type_text { text: "..." }`, which writes directly into the PTY master descriptor (`pty_fd`), seamlessly driving interactive shells and curses-like terminal utilities.

### 2.3 Secret Masking (`ui_textbox_set_secret`)
Widgets marked as secret (e.g., password inputs) automatically redact their `value` field to `"[REDACTED]"` in accessibility tree responses. Only callers holding the explicit `CAP_WINDOW_SECRET` capability can inspect the underlying plaintext or replace its content.

### 2.4 Callback-Driven Action Dispatch
Accessibility actions execute through `libui`'s native widget code paths:
- `click`: Dispatches the widget's registered `on_click` callback.
- `set_text`: Updates the buffer and fires `on_change`.
- `toggle`: Flips checkbox/radio state and triggers change listeners.
- `focus`: Focuses the widget in the window's focus tree.

Actions are never simulated by synthesising synthetic mouse movement or guessing window coordinates, preventing misclicks caused by window movement or frame layout changes.

---

## 3. Window Ownership & Capabilities in `wm`

### 3.1 Kernel Peer Identity Authentication
When a graphical application connects to `/run/display.sock`, the compositor queries `SYS_SOCK_PEERINFO(fd, &peer)`. This guarantees that the process ID (`pid`) and agent sandbox ID (`agent_id`) are immutable and authenticated by the kernel. The compositor resolves the process's executable binary path (e.g., `/bin/files.elf` -> `"files"`) to establish window ownership.

### 3.2 Agent Window Capabilities
Agent sandboxes enforce four discrete window capabilities:
- **`CAP_WINDOW_OBSERVE(allowlist)`**: Grants permission to inspect the window hierarchy and query accessibility trees of applications matching the allowlist.
- **`CAP_WINDOW_ACT(allowlist)`**: Grants permission to dispatch accessibility actions (`click`, `set_text`, etc.) to applications matching the allowlist.
- **`CAP_WINDOW_SECRET`**: Grants permission to read and modify sensitive or secret inputs (e.g., password textboxes). Defaults to off and is never inherited implicitly.
- **`CAP_WINDOW_RAW`**: Permits injecting unmanaged hardware mouse clicks and raw keyboard scancodes, or taking unrestricted screen captures. Defaults to off.

#### Allowlist Enforcement
Allowlists are configured as comma-separated executable basenames:
```bash
agentctl create docbot --window-observe files,uidemo --window-act files
```
If `docbot` attempts to read the accessibility tree of `term.elf`, the compositor rejects the request with `-EPERM`. All capability checks are enforced strictly by the compositor, not by the client application.

### 3.3 Audit Logging
All accessibility actions executed by agents generate structured entries in the kernel's SHA-256 cryptographic audit chain (`AUDIT_EVENT_WINDOW_ACTION`, event ID 14). The audit entry records the caller's `agent_id`, target window ID, target app basename, node role, action name, and a SHA-256 hash of the action arguments (ensuring argument confidentiality).

---

## 4. UI Bridge Daemon (`userland/uibridge.c`)

The UI Bridge daemon bridges the Intent Bus (`/run/intent.sock`) and the Window Manager display server (`/run/display.sock`). It runs as a `respawn` service from `/etc/init.conf`.

### 4.1 Privileged Client Registration
To interact with the compositor on behalf of other agents, `uibridge` registers as a privileged client via `WIN_MSG_PRIVILEGED_REGISTER`. The compositor verifies that the connecting process is an unconfined system process residing in the init process tree (`agent_id == 0`). Unprivileged or agent processes attempting this registration are rejected immediately.

### 4.2 Provided Intent Bus Skills
| Skill | Input Parameters | Output | Description |
| :--- | :--- | :--- | :--- |
| `ui.list_windows` | `{}` | `{ windows: [...] }` | Enumerates all visible windows, their window IDs, titles, application basenames, and owning agent IDs. |
| `ui.get_tree` | `{ window, root?, depth? }` | `{ tree: "..." }` | Returns the accessibility tree formatted as compact indented text. |
| `ui.act` | `{ window, node, action, args? }` | `{ ok: bool, new_state: ... }` | Executes an action on a target widget node. |
| `ui.launch` | `{ app: string, args?: [...] }` | `{ pid: int, window_id: int }` | Launches an allowlisted application inside the caller's agent sandbox. |
| `ui.wait` | `{ window, until: {...}, timeout_ms }` | `{ matched: bool }` | Blocks until a specific widget achieves a target state or value. |
| `ui.focus_window`| `{ window: int }` | `{ ok: bool }` | Brings a window to the top of the Z-order and focuses it. |

### 4.3 Compact Indented Tree Format
To maximize LLM reasoning efficiency and minimize token consumption, `uibridge` transforms JSON accessibility trees into a concise, indentation-based syntax:
```text
Window #1 [files] "File Manager" (100, 80, 500, 360)
  Toolbar [box]
    Button #2 "Back" enabled
    Button #3 "Forward" disabled
    Button #4 "Refresh" enabled
  SplitPane [box]
    ListBox #5 "Folders" items=4 selected=1
      ListItem #6 "Documents" selected
      ListItem #7 "Downloads"
    ListBox #8 "Files" items=3 selected=0
      ListItem #9 "notes.txt" (2.4 KB) selected
      ListItem #10 "budget.csv" (14 KB)
```

### 4.4 Sandbox Inheritance on App Launch
When an agent calls `ui.launch`, `uibridge` does not launch the process with its own elevated privileges. Instead, it forks a child process, invokes `agent_attach(caller_agent_id)` inside the child, and then calls `execv`. The launched application strictly inherits the caller's filesystem, network, token, and window capability sandboxes.

---

## 5. Compositor Trusted Approval UI

For sensitive Intent Bus skills (e.g., `fs.delete`, external device access, elevated configuration changes), `intentd` requests human confirmation via a trusted compositor channel (`/run/approval.sock`).

```
+--------------------------------------------------------------+
| [!] AGENT APPROVAL REQUIRED                                  |
+--------------------------------------------------------------+
| Agent: sysbot (ID 2)                                         |
| Skill: fs.delete                                             |
| Args:  path=/etc/network.conf                                |
| Auto-deny in: 28s                                            |
|                                                              |
|   [ Deny (Esc) ]    [ Allow Once (Enter) ]   [ Allow Session ]|
+--------------------------------------------------------------+
```

### 5.1 Protocol & Modal Rendering
- **Server Endpoint**: The compositor hosts `/run/approval.sock` and accepts requests from `intentd`.
- **Compositor Modal Overlay**: When a request arrives, `wm` pauses normal window event routing and renders a modal card directly onto the backbuffer at the center of the display.
- **Input Controls**:
  - `Deny` (Esc / Key 0): Immediately aborts the invocation with `ERR_DENIED`.
  - `Allow Once` (Enter / Space / Key 1): Authorizes only the current single invocation.
  - `Allow Session` (Key 2): Authorizes all subsequent calls to this skill for the remainder of the agent's lifecycle.
- **Auto-Deny Countdown**: If the user does not respond within 30 seconds, the modal automatically closes and returns an explicit denial.
- **Headless Fallback**: If the graphical compositor is not running (e.g., text mode boot), `intentd` falls back cleanly to the CLI approval queue (`intentctl list_approvals`, `intentctl approve <id>`, `intentctl deny <id>`).

---

## 6. Visible Control Indicators & Safety Mechanisms

To ensure complete transparency during autonomous operation, the window manager overlays visual indicators whenever an agent interacts with the desktop.

### 6.1 Agent Border Coloring & Title Badges
- **Border Chromas**: The window frame border dynamically changes to a designated color corresponding to the active agent ID:
  - Agent 1: Indigo (`#6366F1`)
  - Agent 2: Emerald (`#10B981`)
  - Agent 3: Amber (`#F59E0B`)
  - Agent 4: Rose (`#F43F5E`)
  - Agent 5: Violet (`#8B5CF6`)
  - Agent 6: Cyan (`#06B6D4`)
- **Titlebar Badge**: The window titlebar appends `[Agent: <name>]` while an agent is executing actions on that window.

### 6.2 Widget Target Bounding Box
When an agent invokes `ui.act` on a specific widget, the compositor highlights the widget's exact bounding box with a 2-pixel colored border for 500ms, giving the user direct visual feedback of the agent's focus.

### 6.3 Human-Wins Collision Detection
Human control strictly preempts autonomous actions. Whenever physical hardware mouse motion, mouse clicks, or keyboard events are detected on a window, the compositor updates `window->last_hardware_input_time`. If an agent attempts to dispatch an action within 500ms of human input, the action is immediately aborted and rejected with `ERR_AGENT_ABORTED`.

---

## 7. Unblockable Human Killswitch

The ultimate safety mechanism in MyOS is the **Hardware-Level Agent Killswitch**.

```
+------------------------------------------------------------------------------------------------------+
| [EMERGENCY STOP: ALL AGENTS SUSPENDED - Press Ctrl+Alt+Esc to resume]                                |
+------------------------------------------------------------------------------------------------------+
```

### 7.1 Kernel Interrupt Handling
- **Trigger**: `Ctrl + Alt + Esc` intercepted directly within the PS/2 keyboard interrupt service routine (`src/keyboard.c`).
- **Interrupt Precedence**: Executed at IRQ1 level before scancodes reach TTY layers, PTY buffers, or userland message queues.
- **Kernel State Transition**: Calls `agent_killswitch_toggle()`:
  - Transitions all running agents to `AGENT_STATE_SUSPENDED`.
  - Rejects any agent creation or skill invocation attempts with `-EBUSY`.
  - Logs `AUDIT_EVENT_KILLSWITCH` to the cryptographic audit chain.
  - Emits serial notification: `[KILLSWITCH] Agents paused, press Ctrl+Alt+Esc again to resume`.

### 7.2 Compositor Emergency Stop Banner
The compositor checks `agent_killswitch_state()` on every frame. When active:
- A high-visibility, 22-pixel red banner (`#EF4444`) with white text is rendered at the very top of the screen (`0, 0, screen_w, 22`).
- All agent window actions and background UI bridge operations are completely halted until the human presses `Ctrl+Alt+Esc` again.

---

## 8. The Agent Dashboard (`userland/agentpanel.c`)

The Agent Dashboard provides a centralized GUI management hub for monitoring and controlling all agent activity.

- **System Header**: Displays active agent count, total token consumption, and an interactive Killswitch Toggle button.
- **Preset Launchers**: Single-click cards to launch predefined autonomous workflows from `/etc/agents.d/*.conf`:
  - **`organizer.conf`**: Cleans up directories, formats project logs, and summarizes workspace files.
  - **`explainer.conf`**: Reads documentation and code symbols to provide contextual interactive explanations.
  - **`terminal-helper.conf`**: Operates terminal workflows and runs build/test scripts.
- **Running Agents Table**: Live tabular view of all active agents, parent PIDs, memory usage, token budgets, and individual termination controls.
- **Ask Bar (`Super+Space`)**: A universal input bar at the bottom of the dashboard. Pressing `Super+Space` focuses the prompt bar, forwarding conversational instructions directly to `modeld` and dispatching tasks to autonomous workers.

