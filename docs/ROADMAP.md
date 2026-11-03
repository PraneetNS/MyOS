# MyOS Future Roadmap & Good First Issues

Welcome to the MyOS project roadmap! This document outlines upcoming milestones and areas where external contributors can make high-impact contributions.

---

## 🎯 Short-Term Milestones (Phase 7: Developer Experience & Multi-Agent Collaboration)

- **Hierarchical Agent IPC**: Enable child agents to establish private IPC channels back to parent agents with strict capability delegation.
- **Dynamic Port Forwarding GUI**: Visual network connection inspector in `agentpanel` displaying active TCP/UDP sockets and bytes transferred in real time.
- **Enhanced FAT16 Performance**: Multi-sector read/write batching in ATA PIO driver to boost disk throughput.
- **Local Model Int4 Support**: Port 4-bit integer quantization to `userland/llm/` for reduced memory usage.

---

## 🚀 Completed: Phase 8 (x86-64 Long Mode & Modern Hardening)

- **x86-64 Transition**: Booted in 64-bit long mode with 4-level paging, fast `syscall` path, 64-bit GDT/IDT/TSS, and unified SysV AMD64 ABI.
- **UEFI Boot**: Hybrid ISO with GRUB-EFI and El Torito boot images running seamlessly with Tianocore OVMF and legacy BIOS.
- **>4GB Memory Support**: Dynamic physical memory manager supporting up to 64GB+ RAM with early memory-mapped refcount arrays (verified at 6GB and 8GB).
- **Modern CPU Hardening**: Hardware NX enforcement, SMEP, SMAP with strict `stac`/`clac` bracketing, and userland mmap ASLR.

---

## 🔮 Future Milestones (Phase 9: Hardware Realism & SMP)

- **Symmetric Multiprocessing (SMP)**:
  - **SMP Spinlock Architecture**:
    - Multi-core CPU synchronization replacing uni-processor `irq_save()`/`irq_restore()` critical sections with fine-grained spinlocks.
    - Ticket spinlocks (`ticket_lock_t` with `owner` and `next` counters) or MCS list-based locks to guarantee FIFO fairness and avoid cache-line bouncing.
    - IRQ-safe spinlock primitives: `spin_lock_irqsave(&lock, &flags)` and `spin_unlock_irqrestore(&lock, flags)` ensuring ISRs cannot preempt and deadlock while holding spinlocks.
    - Deadlock mitigation: bounded spin timeout with lockdep / NMI backtrace reporting if lock acquisition exceeds cycle thresholds.
    - Per-CPU runqueues and load balancing: scheduler queues protected by individual per-CPU runqueue locks, eliminating global scheduler lock contention.
- **NVMe / AHCI Storage**: Native PCIe NVMe block storage driver replacing legacy ATA PIO.
- **Hardware Virtualization Hypervisor Support**: Hardware-assisted ring virtualization (VT-x / AMD-V) for strict agent isolation.

---

## 🌟 Good First Issues for New Contributors

Looking to get involved? Here are several beginner-friendly tasks:

1. **Add new shell built-ins (`userland/sh.c`)**:
   - Implement `alias`, `unalias`, or `history` command support.
2. **Additional POSIX utilities (`userland/`)**:
   - Port or implement lightweight versions of `strings`, `basename`, `dirname`, or `fold`.
3. **TrueType Font Rendering Enhancements (`userland/libgfx/`)**:
   - Add text drop-shadow support or kerning adjustment.
4. **GUI App Improvements (`userland/paint.c`, `userland/view.c`)**:
   - Add a color palette picker to `paint.elf` or image zoom/pan controls to `view.elf`.
5. **Additional Network Tests (`userland/nettest.c`)**:
   - Write tests for ICMP echo payload matching or UDP broadcast packet filtering.

