# Demo 2: Research Assistant (Network + Persistent Memory)

## Overview
The **Researcher** demo demonstrates agent network mediation and untrusted memory handling:
1. Agent launches with principal `researcher`.
2. Requests an allowlisted technical documentation page via `netd` (`net.http` Intent Bus call).
3. `netd` validates the domain allowlist and performs the HTTP GET, labeling the payload with `untrusted: true`.
4. The agent indexes key concepts and stores a summary note in `memd` (`mem.write`).
5. `memd` automatically classifies the record with `trust=agent` (untrusted origin).
6. On a subsequent query, the agent recalls the memory record via `mem.search`.
7. `agentrun` wraps the recalled knowledge with `[UNTRUSTED DATA FROM TOOL mem.search]`, preventing stored prompt injection from hijacking future reasoning.

## How to Run

### Via Make & Headless Automated Recorder:
```bash
make demo-researcher
# or
./tools/record_demo.sh researcher
```

### Manually inside MyOS:
1. Boot the OS with networking enabled:
   ```bash
   python3 tools/testnet_server.py --bg
   qemu-system-i386 -m 128 -drive file=disk.img,format=raw -netdev user,id=net0 -device virtio-net-pci,netdev=net0
   ```
2. Run researcher demo script:
   ```bash
   agentrun --principal researcher --goal "Research local HTTP release notes and index findings"
   ```
3. Inspect memory entries and trust classifications:
   ```bash
   memctl search "release notes"
   ```

