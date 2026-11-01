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
- **IPv4**: Handles TTL decrementation, IPv4 header checksum validation and generation, fragmentation handling, and routing decisions against the kernel route table.
- **ICMP**: Responds to ICMP Echo Requests (ping) and supports ping transmission.

### 2.4 TCP State Machine & UDP
- **TCP**: Full RFC 793 state machine supporting `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`, `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, and `TIME_WAIT`. Handles sequence number tracking, sliding window flow control, retransmissions, and graceful connection teardown.
- **UDP**: Lightweight connectionless datagram sockets with port demultiplexing and checksumming.

---

## 3. Kernel Agent Network Isolation

Agents in MyOS are subject to strict kernel-enforced sandboxing:
- **Default Deny**: By default, agents are blocked from directly opening `AF_INET` or `AF_PACKET` sockets.
- **`CAP_NET_RAW_INET`**: An agent attempting `socket(AF_INET, ...)` without holding `CAP_NET_RAW_INET` fails immediately with `-EACCES`.
- **Audit Logging**: Every denied network socket attempt is permanently recorded in `/proc/agents/log` with an immutable SHA-256 hash-chained entry (`AUDIT_EVENT_NET_DENIED`).
- **Mediation**: All outbound agent network interactions are mediated through `netd` (the Agent Network Gateway daemon) via the Intent Bus.

---

## 4. Hardware Network Killswitch

MyOS features a global hardware-level network killswitch:
- Exposed at `/proc/sys/net/killswitch` and `/proc/net/killswitch`.
- When set to `1` (`echo 1 > /proc/sys/net/killswitch`):
  - Ingress and egress queues on all non-loopback devices (`eth0`) are instantly disabled at the driver level.
  - Hardware transmission buffers are flushed, and all inbound network frames are dropped prior to protocol parsing.
  - Loopback (`127.0.0.1` / `lo`) remains operational so internal IPC and system daemons continue uninterrupted.
- When toggled back to `0`, external interface packet flow is safely resumed.

