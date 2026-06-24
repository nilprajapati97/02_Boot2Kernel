# Linux Kernel — Senior Engineer Interview Prep (9+ Years)

A complete, in-depth study pack of **25 deep-dive documents** for senior Linux kernel
engineer interviews at **Google, NVIDIA, Qualcomm, and AMD**.

Each document is engineered so you can **talk for 30+ minutes** on the topic with
confidence — covering **Why / When / Where / How**, data-structure diagrams, annotated
C, company-specific angles, a war story, and likely interviewer follow-ups.

> **Kernel baseline:** v6.6+ (EEVDF scheduler, folios, maple-tree VMA store, modern
> DMA/IOMMU APIs). Legacy behavior (e.g. CFS, VMA rbtree) is called out where it still
> comes up in interviews.

---

## How to use this pack

1. **First pass (recall):** Read only the **TL;DR cheat sheet** at the top of each doc.
2. **Second pass (depth):** Read the full **Why / When / Where / How** + diagrams.
3. **Third pass (performance):** Practice the **30-Min Talk Track** out loud, then have
   someone fire the **Interviewer Follow-ups** at you.
4. **Night before:** Re-read all 25 TL;DRs + the **War Story** of each doc — war stories
   are what separate a 9-year engineer from a textbook answer.

### Legend used in every document

| Section | Purpose |
|---------|---------|
| **TL;DR Cheat Sheet** | 30-second rapid recall: key structs, APIs, one-liners |
| **Why** | The problem it solves, design rationale, trade-offs |
| **When** | Triggers / when to use vs alternatives |
| **Where** | Source files, structs, subsystem boundaries |
| **How** | The deep step-by-step mechanics — your core narrative |
| **Diagrams** | ASCII data-structure layouts + Mermaid flow/sequence |
| **Annotated C** | Real kernel structs/APIs with commentary |
| **Company Angle** | Why each target company probes this |
| **War Story** | A concrete root-cause narrative you can tell |
| **Interviewer Follow-ups** | Likely follow-ups with crisp answers |
| **30-Min Talk Track** | Minute-by-minute speaking outline |

---

## Index

### 1. Memory Management
| # | Topic | Doc |
|---|-------|-----|
| 1 | Page table walk (4/5-level), TLB, huge pages | [01-page-table-walk.md](01-memory-management/01-page-table-walk.md) |
| 2 | Buddy allocator ↔ SLUB | [02-buddy-slab-allocator.md](01-memory-management/02-buddy-slab-allocator.md) |
| 3 | `kmalloc` vs `vmalloc` vs `kmem_cache_alloc` vs `alloc_pages` | [03-kmalloc-vmalloc-alloc-pages.md](01-memory-management/03-kmalloc-vmalloc-alloc-pages.md) |
| 4 | Page reclaim: LRU, kswapd, direct reclaim, OOM | [04-page-reclaim-oom.md](01-memory-management/04-page-reclaim-oom.md) |
| 5 | Reverse mapping (rmap) | [05-reverse-mapping-rmap.md](01-memory-management/05-reverse-mapping-rmap.md) |

### 2. Concurrency & Synchronization
| # | Topic | Doc |
|---|-------|-----|
| 6 | Spinlock vs mutex vs semaphore vs rwsem vs RCU | [06-locking-primitives-compare.md](02-concurrency-sync/06-locking-primitives-compare.md) |
| 7 | RCU deep dive | [07-rcu-deep-dive.md](02-concurrency-sync/07-rcu-deep-dive.md) |
| 8 | Memory barriers & LKMM | [08-memory-barriers-lkmm.md](02-concurrency-sync/08-memory-barriers-lkmm.md) |
| 9 | Per-CPU variables | [09-per-cpu-variables.md](02-concurrency-sync/09-per-cpu-variables.md) |
| 10 | Deadlocks & lockdep | [10-deadlock-lockdep.md](02-concurrency-sync/10-deadlock-lockdep.md) |

### 3. Interrupts & Bottom Halves
| # | Topic | Doc |
|---|-------|-----|
| 11 | Top vs bottom half; softirq/tasklet/workqueue/threaded IRQ | [11-top-bottom-half.md](03-interrupts/11-top-bottom-half.md) |
| 12 | Full IRQ path: GIC/APIC, irq_domain, MSI/MSI-X | [12-irq-path-gic-apic-msi.md](03-interrupts/12-irq-path-gic-apic-msi.md) |
| 13 | Why you can't sleep in IRQ context | [13-no-sleep-in-irq.md](03-interrupts/13-no-sleep-in-irq.md) |

### 4. Scheduling
| # | Topic | Doc |
|---|-------|-----|
| 14 | CFS → EEVDF scheduler | [14-cfs-eevdf-scheduler.md](04-scheduling/14-cfs-eevdf-scheduler.md) |
| 15 | Load balancing across sched domains & NUMA | [15-load-balancing-numa.md](04-scheduling/15-load-balancing-numa.md) |
| 16 | Preemption models | [16-preemption-models.md](04-scheduling/16-preemption-models.md) |

### 5. Drivers, DMA & Device Model
| # | Topic | Doc |
|---|-------|-----|
| 17 | Character device driver | [17-char-device-driver.md](05-drivers-dma-devicemodel/17-char-device-driver.md) |
| 18 | DMA, IOMMU & cache coherency | [18-dma-iommu-coherency.md](05-drivers-dma-devicemodel/18-dma-iommu-coherency.md) |
| 19 | Device tree & driver matching | [19-device-tree-of-match.md](05-drivers-dma-devicemodel/19-device-tree-of-match.md) |
| 20 | Linux device model & sysfs | [20-device-model-sysfs.md](05-drivers-dma-devicemodel/20-device-model-sysfs.md) |

### 6. Debugging & Performance
| # | Topic | Doc |
|---|-------|-----|
| 21 | Debugging panic/oops | [21-panic-oops-debug.md](06-debugging-performance/21-panic-oops-debug.md) |
| 22 | ftrace / perf / kprobes / eBPF / tracepoints | [22-ftrace-perf-ebpf.md](06-debugging-performance/22-ftrace-perf-ebpf.md) |
| 23 | Kernel memory leaks (kmemleak/KASAN/slub_debug) | [23-kernel-memory-leak.md](06-debugging-performance/23-kernel-memory-leak.md) |
| 24 | High latency / soft lockups | [24-latency-soft-lockups.md](06-debugging-performance/24-latency-soft-lockups.md) |

### 7. System Architecture
| # | Topic | Doc |
|---|-------|-----|
| 25 | Boot sequence: power-on → init/systemd | [25-boot-sequence.md](07-system-architecture/25-boot-sequence.md) |

---

## Company emphasis matrix

| Company | Focus Areas | Must-nail docs |
|---------|-------------|----------------|
| **Google** | eBPF, scheduling, cgroups, networking, large-scale debugging | 7, 14, 22, 24 |
| **NVIDIA** | DMA/IOMMU, PCIe, GPU drivers, RCU/barriers, real-time | 7, 8, 12, 18 |
| **Qualcomm** | ARM64 internals, device tree, PM/runtime PM, SoC bring-up | 1, 18, 19, 25 |
| **AMD** | NUMA, cache coherency, PCIe, MM, multi-core scaling | 2, 8, 15, 18 |

---

## Senior-level reminders

- Interviewers expect **war stories** — concrete bugs you root-caused, patches you
  upstreamed, and trade-off discussions, not textbook definitions.
- Be ready to **whiteboard data structures** (`struct page`, `task_struct`, RCU lists)
  and reason about **lock-free** algorithms.
- Always state **trade-offs** explicitly ("I'd use X here because Y, accepting cost Z").
- When unsure, **reason from first principles** about hardware (TLB, cache lines,
  memory ordering) — that's the senior signal.
