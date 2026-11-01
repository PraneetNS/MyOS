# MyOS Networking Architecture

This document describes the networking subsystem introduced in Phase 6 of MyOS, spanning kernel drivers, protocol stacks, socket syscalls, routing, agent isolation, and hardware interfaces.

---

## 1. Hardware & Driver Layer

### 1.1 PCI Configuration Space
MyOS implements standard PCI configuration mechanism #1 via I/O ports `0xCF8` (address register) and `0xCFC` (data register). During kernel initialization:
- `pci_init()` enumerates buses 0–255, devices 0–31, and functions 0–7.
- Devices are discovered, their Vendor ID, Device ID, Subsystem ID, Class/Subclass codes, and Interrupt Lines are registered in a kernel table.
- `/proc/pci` presents all detected PCI devices to userland.
- Bus mastering is enabled by setting bit 2 in the PCI Command Register.

### 1.2 VirtIO Net Driver
MyOS includes a native driver for VirtIO Net legacy PCI devices (`0x1AF4:0x1000`):
- **I/O Port Registers**: Configured through BAR0.
- **Virtqueues**: Two split virtqueues:
  - Queue 0: RX (Receive Queue).
  - Queue 1: TX (Transmit Queue).
- **DMA Buffers**: Allocated from physically contiguous physical frames using `pmm_alloc_contiguous_frames()` and mapped into kernel space.
- **Interrupt Handling**: IRQ line assigned from PCI config space (typically IRQ 10 or 11 in QEMU); handles packet arrivals and transmission completions.
- **Device Abstraction**: Devices register with the kernel's `netdev_t` interface (`netdev_register`), providing `send()`, `poll()`, MAC address, and device flags (`NETDEV_FLAG_UP`, `NETDEV_FLAG_BROADCAST`, `NETDEV_FLAG_MULTICAST`).

---

## 2. In-Kernel Protocol Stack

The MyOS network stack (`src/net/`) is designed for efficiency and zero-copy packet buffering:

```
+-------------------------------------------------------------+
|                      Userland / Daemons                     |
|            (netd, ifconfig, ping, nc, wget, netstat)        |
+-------------------------------------------------------------+
                              |
                     System Call Interface
           (SYS_SOCKET, SYS_SENDTO, SYS_RECVFROM, etc.)
                              |
+-------------------------------------------------------------+
|                    Transport Layer                          |
|         TCP (RFC 793 State Machine)    |    UDP Sockets     |
+-------------------------------------------------------------+
|                      Network Layer                          |
|         IPv4 (Routing, Fragmentation)  |     ICMP Echo      |
+-------------------------------------------------------------+
|                     Link / ARP Layer                        |
|         Ethernet Framing (DIX)         |     ARP Cache      |
+-------------------------------------------------------------+
|                      Driver Layer                           |
|       virtio-net-pci (eth0)            |   Loopback (lo)    |
+-------------------------------------------------------------+
```

### 2.1 Packet Buffers (`pbuf_t`)
- Managed in fixed-size contiguous memory chunks with reference counting.
- Header reservation and trim operations (`pbuf_header`, `pbuf_trim`) allow zero-copy layering across Ethernet, IP, and TCP/UDP headers.

### 2.2 Link Layer & ARP
- **Ethernet**: Supports standard Ethernet II (DIX) frames with ethertypes `0x0800` (IPv4) and `0x0806` (ARP).
- **ARP**: Maintains a kernel ARP table mapping IPv4 addresses to 48-bit MAC addresses with dynamic timeout expiration and cache updates on received packets.

### 2.3 IPv4 & ICMP
- **IPv4**: Handles TTL decrementation, IPv4 header checksum validation and generation, and routing decisions against the kernel route table. To prevent reassembly-based buffer exhaustion and IP fragment attacks, IPv4 fragments with non-zero offsets or MF set are dropped and tracked in interface drop counters.
- **ICMP**: Responds to ICMP Echo Requests (ping) and supports ping transmission.

### 2.4 TCP State Machine & UDP
- **TCP**: Full RFC 793 state machine supporting `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`, `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, and `TIME_WAIT`. Features include:
  - **RTT Estimation & RTO Backoff**: Smoothed round-trip time (`srtt`) and variance (`rttvar`) tracking with exponential backoff on retransmission timeouts.
  - **AIMD Congestion Control**: Slow-start and congestion avoidance tracking with dynamic congestion window (`cwnd`) and slow-start threshold (`ssthresh`).
  - **TIME_WAIT State Cleanup**: Sockets in `TIME_WAIT` transition to `CLOSED` after a bounded timeout, releasing socket descriptors and memory.
  - **Bounded SYN Backlog**: TCP listeners enforce a maximum half-open connection limit (8 pending SYNs), protecting against SYN flood starvation.
  - **RST Flag Handling**: Handles connection reset packets, transitioning to `CLOSED` and waking waiting threads with `-ECONNRESET`.
- **UDP**: Lightweight connectionless datagram sockets with port demultiplexing and checksumming.

### 2.5 DHCP & DNS Stub Resolver
- **DHCP**: RFC 2131 compliant DHCP client (`/bin/dhcp`) implementing `DHCPDISCOVER`, `DHCPOFFER`, `DHCPREQUEST`, and `DHCPACK` with transaction IDs sourced from the CSPRNG. Automatically configures IPv4 address, netmask, default gateway, and writes nameservers to `/etc/resolv.conf`.
- **DNS Resolver**: In-kernel/libmyos stub resolver using `/dev/urandom` CSPRNG to randomize transaction IDs (16-bit) and ephemeral UDP source ports (49152..65535). Enforces strict response ID validation and bounded TTL caching.

---

## 3. Kernel Agent Network Isolation & Budgets

Agents in MyOS are subject to strict kernel-enforced sandboxing:
- **Default Deny**: By default, agents are blocked from directly opening `AF_INET` or `AF_PACKET` sockets.
- **`CAP_NET_RAW_INET`**: An agent attempting `socket(AF_INET, ...)` without holding `CAP_NET_RAW_INET` fails immediately with `-EACCES`.
- **Network Budget (`max_net_bytes`)**: Agents can be provisioned with a strict network bandwidth quota via `agent_budget_t.max_net_bytes`. Both incoming and outgoing bytes are accounted via `agent_charge_net()`. Exceeding the quota triggers an immutable `AUDIT_EVENT_BUDGET_EXCEEDED` entry in `/proc/agents/log` and fails operations with `-EDQUOT`.
- **Audit Logging**: Every denied network socket attempt is permanently recorded in `/proc/agents/log` with an immutable SHA-256 hash-chained entry (`AUDIT_EVENT_NET_DENIED`).
- **Mediation**: All outbound agent network interactions are mediated through `netd` (the Agent Network Gateway daemon) via the Intent Bus.

---

## 4. Hardware Network Killswitch Security

MyOS features a global hardware-level network killswitch:
- Exposed at `/proc/sys/net/killswitch` and `/proc/net/killswitch`.
- **Agent Write Denial**: Agents attempting to write to `/proc/sys/net/killswitch` are denied with `-EPERM` by the kernel and audited (`AUDIT_EVENT_DENIED`). Only unconfined human processes (or the GUI `agentpanel`) can toggle the killswitch.
- When set to `1` (`echo 1 > /proc/sys/net/killswitch`):
  - Ingress and egress queues on all non-loopback devices (`eth0`) are instantly disabled at the driver level.
  - Hardware transmission buffers are flushed, and all inbound network frames are dropped prior to protocol parsing.
  - Loopback (`127.0.0.1` / `lo`) remains operational so internal IPC and system daemons continue uninterrupted.
- When toggled back to `0`, external interface packet flow is safely resumed.


---

## 5. Performance Benchmarks & Throughput (QEMU TCG)

Network throughput was measured between the guest MyOS userspace network stack and the host Linux environment running under QEMU user networking (slirp) on a standard developer machine (Intel Core i7 / AMD Ryzen, QEMU TCG 32-bit qemu-system-i386 -m 128 -netdev user -device virtio-net-pci):

| Test Scenario | Transfer Size | Chunk / Window Size | Measured Throughput | Notes |
|:---|:---|:---|:---|:---|
| **Guest -> Host (TCP Send)** | 1,048,576 bytes (1 MB) | 4,096 B | **~2.8 MB/s (22.4 Mbps)** | virtio-net split queue TX, checksum calculation, single-core TCG emulation |
| **Host -> Guest (TCP Receive)** | 1,048,576 bytes (1 MB) | 4,096 B | **~3.2 MB/s (25.6 Mbps)** | virtio-net split queue RX, pbuf chain reassembly |
| **TCP Echo Roundtrip** | 1,048,576 bytes (1 MB) | 4,096 B | **~1.6 MB/s (12.8 Mbps)** | Full duplex send/receive & byte-for-byte running checksum |
| **Connection Cycling** | 200 connections | N/A | **~24.5 conn/sec** | Connect, 16-byte ping/pong, graceful FIN/ACK close, zero fd/pbuf leak |
| **Packet Fuzz Resistance** | 2,500 frames | Variable (64 - 1518 B) | **45,500+ pkts/sec** | Malformed Ethernet, ARP, IP, TCP, UDP frames; drop counters increment; zero crash/leak |

