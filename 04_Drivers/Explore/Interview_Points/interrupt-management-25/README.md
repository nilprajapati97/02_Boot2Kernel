# Linux Interrupt Management — Senior Engineer Deep Dive (25 Questions)

A focused, in-depth study pack of **25 Interrupt Management deep-dive documents** for senior
(9+ years) Linux kernel engineer interviews at **Google, NVIDIA, Qualcomm, and AMD**.

Each document is engineered so you can **talk for 30+ minutes** on the topic — covering
**Why / When / Where / How**, data-structure diagrams, annotated kernel C, company-specific
angles, a war story, and likely interviewer follow-ups.

> **Kernel baseline:** v6.6+ (GICv3/v4 + ITS, hierarchical `irq_domain`, CMWQ workqueues,
> threaded IRQs, managed/auto IRQ affinity, threaded NAPI, PREEMPT_RT mostly mainline).
> Legacy behavior is called out where it still comes up.

> **Companion set:** interrupt fundamentals (top vs bottom half, the IRQ path overview,
> why you can't sleep in IRQ context) live in [../03-interrupts/](../03-interrupts).
> This set goes **deeper per subsystem** with minimal overlap; cross-links are provided.

---

## How to use this pack

1. **First pass (recall):** read only the **TL;DR cheat sheet** at the top of each doc.
2. **Second pass (depth):** read the full **Why / When / Where / How** + diagrams.
3. **Third pass (performance):** say the **30-Min Talk Track** out loud; have someone fire the
   **Interviewer Follow-ups** at you.
4. **Night before:** re-read all 25 TL;DRs + every **War Story** — war stories separate a
   9-year engineer from a textbook answer.

### Legend (every document uses these sections)

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

### 1. Interrupt Controllers
| # | Topic | Doc |
|---|-------|-----|
| 1 | GIC architecture (v2/v3/v4, ITS, LPIs) | [01-gic-architecture.md](01-controllers/01-gic-architecture.md) |
| 2 | x86 APIC (Local/IO-APIC/x2APIC) | [02-x86-apic.md](01-controllers/02-x86-apic.md) |
| 3 | irq_domain & hierarchical domains | [03-irq-domain-hierarchy.md](01-controllers/03-irq-domain-hierarchy.md) |
| 4 | MSI/MSI-X internals & MSI domains | [04-msi-msix-internals.md](01-controllers/04-msi-msix-internals.md) |
| 5 | IPIs (inter-processor interrupts) | [05-ipi-inter-processor.md](01-controllers/05-ipi-inter-processor.md) |

### 2. Generic IRQ Core
| # | Topic | Doc |
|---|-------|-----|
| 6 | Generic IRQ layer (irq_desc/irq_chip/irq_data) | [06-generic-irq-layer.md](02-generic-irq-core/06-generic-irq-layer.md) |
| 7 | Flow handlers (level/edge/fasteoi/percpu) | [07-flow-handlers.md](02-generic-irq-core/07-flow-handlers.md) |
| 8 | Sparse IRQs & irq_desc management | [08-sparse-irq-desc.md](02-generic-irq-core/08-sparse-irq-desc.md) |
| 9 | request_irq / threaded_irq / free_irq lifecycle | [09-request-irq-lifecycle.md](02-generic-irq-core/09-request-irq-lifecycle.md) |
| 10 | Shared IRQs + spurious/storm detection | [10-shared-spurious-storms.md](02-generic-irq-core/10-shared-spurious-storms.md) |

### 3. Bottom Halves
| # | Topic | Doc |
|---|-------|-----|
| 11 | Softirqs deep dive | [11-softirqs-deep-dive.md](03-bottom-halves/11-softirqs-deep-dive.md) |
| 12 | Tasklets internals & deprecation (BH workqueues) | [12-tasklets-bh-workqueues.md](03-bottom-halves/12-tasklets-bh-workqueues.md) |
| 13 | Workqueues / CMWQ deep dive | [13-workqueues-cmwq.md](03-bottom-halves/13-workqueues-cmwq.md) |
| 14 | Threaded IRQs internals | [14-threaded-irqs-internals.md](03-bottom-halves/14-threaded-irqs-internals.md) |

### 4. Affinity & Performance
| # | Topic | Doc |
|---|-------|-----|
| 15 | IRQ affinity & irqbalance | [15-irq-affinity-irqbalance.md](04-affinity-performance/15-irq-affinity-irqbalance.md) |
| 16 | NAPI & interrupt mitigation | [16-napi-interrupt-mitigation.md](04-affinity-performance/16-napi-interrupt-mitigation.md) |
| 17 | Interrupt coalescing / moderation | [17-interrupt-coalescing.md](04-affinity-performance/17-interrupt-coalescing.md) |
| 18 | CPU isolation for IRQs | [18-cpu-isolation-irqs.md](04-affinity-performance/18-cpu-isolation-irqs.md) |

### 5. Context, Masking & State
| # | Topic | Doc |
|---|-------|-----|
| 19 | IRQ context & masking | [19-irq-context-masking.md](05-context-masking-state/19-irq-context-masking.md) |
| 20 | disable_irq vs mask, lazy disable, synchronize_irq | [20-disable-vs-mask-lazy.md](05-context-masking-state/20-disable-vs-mask-lazy.md) |
| 21 | IRQ time accounting & statistics | [21-irq-accounting-statistics.md](05-context-masking-state/21-irq-accounting-statistics.md) |

### 6. Real-Time & Latency
| # | Topic | Doc |
|---|-------|-----|
| 22 | PREEMPT_RT & interrupts | [22-preempt-rt-interrupts.md](06-realtime-latency/22-preempt-rt-interrupts.md) |
| 23 | Interrupt latency (measure & reduce) | [23-interrupt-latency.md](06-realtime-latency/23-interrupt-latency.md) |

### 7. Power & Special
| # | Topic | Doc |
|---|-------|-----|
| 24 | Wakeup interrupts & PM | [24-wakeup-interrupts-pm.md](07-power-special/24-wakeup-interrupts-pm.md) |
| 25 | Per-CPU interrupts (PPIs) | [25-percpu-irqs-ppi.md](07-power-special/25-percpu-irqs-ppi.md) |

---

## Company emphasis matrix

| Company | Interrupt focus | Must-nail docs |
|---------|-----------------|----------------|
| **Google** | NAPI/softirq networking, RPS/RFS, IRQ affinity at scale, threaded NAPI, observability | 11, 15, 16, 17 |
| **NVIDIA** | MSI-X (GPU), IPIs, irq_domain, threaded IRQs, real-time latency | 4, 5, 14, 23 |
| **Qualcomm** | GIC (ARM), wakeup IRQs/PM, threaded IRQs, PREEMPT_RT, PPIs, DT interrupts | 1, 22, 24, 25 |
| **AMD** | x86 APIC, MSI-X multi-queue (NVMe/GPU), IPI/TLB shootdown, many-core scaling | 2, 4, 5, 15 |

---

## Senior-level reminders

- Interviewers want **war stories** — concrete bugs you root-caused (IRQ storms, MSI-X affinity
  imbalance, softirq starvation, missed wakeups, RT latency spikes), patches you upstreamed, and
  **trade-off** discussions.
- Be ready to **whiteboard data structures**: `irq_desc`, `irq_chip`, `irq_data`, `irqaction`,
  the `irq_domain` hierarchy, `napi_struct`, the softirq pending mask.
- Always reason from **hardware first**: how the controller signals the CPU, masking vs disabling,
  EOI ordering, per-CPU vs shared lines, MSI as a memory write.
- State trade-offs explicitly ("I'd thread this IRQ because Y, accepting latency cost Z").
