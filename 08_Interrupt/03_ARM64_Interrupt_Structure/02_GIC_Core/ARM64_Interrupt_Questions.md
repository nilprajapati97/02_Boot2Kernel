# ARM64 / Linux Kernel Interrupt Handling — Five Core Design Questions

A deep-dive into the fundamental design questions every interrupt controller
and OS must answer, explained for the **ARM64 + GICv3 + Linux** stack.

The five questions:

1. **Which CPU should receive the interrupt?**
2. **What if multiple interrupts occur simultaneously?**
3. **Which interrupt has higher priority?**
4. **How does the CPU know which interrupt occurred?**
5. **How do we support thousands of interrupts?**

---

## Q1. Which CPU Should Receive the Interrupt?

### The Problem

On an SMP system with N CPUs, when a device (GPIO, UART, NIC, NVMe) raises an
interrupt, *somebody* has to decide which core handles it. Wrong answer wastes
cache, breaks NUMA locality, or causes thundering-herd wakeups.

### The Mechanism in GICv3: `GICD_IROUTER<n>`

For every SPI (INTID 32–1019), the Distributor has a **64-bit routing
register**:

```
GICD_IROUTER<n> @ GICD_BASE + 0x6000 + (n * 8)

┌────────┬────────┬────────┬────────┬────────┬───────┐
│ 63:40  │ 39:32  │ 31:24  │ 23:16  │ 15:8   │ 7:0   │
│ Aff3   │  IRM   │ Aff2   │ Aff1   │ Aff0   │ Rsvd  │
└────────┴────────┴────────┴────────┴────────┴───────┘

IRM (bit 31):
  0 → "1 of 1" mode: route to the exact CPU whose MPIDR_EL1 matches
      Aff3.Aff2.Aff1.Aff0
  1 → "1 of N" mode: GIC picks any participating CPU from the affinity
      class (hardware load-balance)
```

The GIC matches the routing register against each CPU's `MPIDR_EL1`
(Multiprocessor Affinity Register). MPIDR is read at boot via
`cpu_logical_map[cpu]` and stored in `cpu_logical_map[]`.

### Three Layers That Decide Affinity

#### Layer 1 — Boot Default (GIC driver)

`drivers/irqchip/irq-gic-v3.c : gic_dist_init()` programs every SPI to CPU0:

```c
for (i = 32; i < GIC_LINE_NR; i++) {
    affinity = gic_mpidr_to_affinity(cpu_logical_map(0));
    gic_write_irouter(affinity, base + GICD_IROUTER + i * 8);
}
```

#### Layer 2 — `request_irq()` Time

In `kernel/irq/manage.c : irq_setup_affinity()`:

```c
if (!__irq_can_set_affinity(desc))         return;
if (irqd_affinity_is_managed(&desc->irq_data)) return;  // pre-set

cpumask_and(&mask, irq_default_affinity, cpu_online_mask);
ret = irq_do_set_affinity(&desc->irq_data, &mask, false);
```

`irq_default_affinity` is `cpu_possible_mask` by default and can be tuned via
`/proc/irq/default_smp_affinity`.

#### Layer 3 — Runtime (sysfs / irqbalance)

```bash
# Bitmask form (CPU2 = bit 2)
echo 4 > /proc/irq/56/smp_affinity

# List form
echo 2 > /proc/irq/56/smp_affinity_list
```

Kernel path:

```
write_irq_affinity()
  → irq_set_affinity()
    → irq_do_set_affinity()
      → chip->irq_set_affinity() == gic_set_affinity()
        → val = gic_mpidr_to_affinity(cpu_logical_map(cpu));
          gic_write_irouter(val, base + GICD_IROUTER + hwirq * 8);
```

### Special Cases

| Interrupt Type | Routing |
|---|---|
| **SGI** (0–15, IPIs) | Targeted by `ICC_SGI1R_EL1` write — software picks |
| **PPI** (16–31, per-CPU) | Implicitly local (arch timer, perf, etc.) — no routing needed |
| **SPI** (32–1019) | `GICD_IROUTER<n>` controls it |
| **LPI** (8192+, MSI) | ITS Collection table maps LPI → target GICR (CPU) |

### Managed IRQs (NVMe, network multi-queue)

The kernel pre-pins each hardware queue's IRQ to a specific CPU at allocation
time via `pci_alloc_irq_vectors_affinity()` and marks them `IRQF_NOBALANCING`.
This is why `irqbalance` cannot move them — moving them would defeat the
hardware queue ↔ CPU pairing.

### What Happens on CPU Hotplug

When a CPU goes offline (`echo 0 > /sys/.../cpuX/online`):

1. `irq_migrate_all_off_this_cpu()` walks every IRQ targeting this CPU.
2. For each affected IRQ, picks a new CPU from `cpu_online_mask`.
3. Calls `chip->irq_set_affinity()` → updates `GICD_IROUTER`.
4. Pending interrupts may be redirected via `GICD_ICPENDR` / re-pending.

---

## Q2. What If Multiple Interrupts Occur Simultaneously?

### Reality: Interrupts Are Never "Simultaneous" at the Wire

Even if two devices assert their lines in the same clock cycle, the GICD
serializes them through an internal arbitration network. The "simultaneous"
case really means:

> Multiple INTIDs are in **pending** state at the same time, possibly across
> multiple CPUs.

### GIC Distributor Arbitration

The GICD continuously evaluates all pending bits in `GICD_ISPENDR<n>` against:

```
Eligible if:
  GICD_ISENABLER[INTID] = 1       (interrupt enabled)
  Target CPU's ICC_PMR_EL1 > GICD_IPRIORITYR[INTID]   (not priority-masked)
  Target CPU is not already running same-or-higher priority IRQ
```

Among eligible candidates, it picks the **highest priority** (lowest numeric
value). On ties, lowest INTID wins.

### Per-CPU Serialization on ARM64

Once the GIC delivers an IRQ to a CPU and that CPU acknowledges it:

1. HW sets `PSTATE.I = 1` → further IRQs on that CPU are masked.
2. The CPU runs `el1_irq` / `el0_irq` → `gic_handle_irq()`.
3. `gic_handle_irq()` is a **loop**:

```c
do {
    irqnr = read_sysreg_s(SYS_ICC_IAR1_EL1);
    if (irqnr == ICC_IAR1_EL1_SPURIOUS) break;
    handle_one(irqnr);
    write_sysreg_s(irqnr, SYS_ICC_EOIR1_EL1);
} while (1);
```

So if 5 IRQs are pending **for this CPU**, the loop drains them in priority
order **without** returning to the interrupted task between them. Other CPUs
drain *their* pending IRQs in parallel.

### Shared Lines (Level-Triggered, Multiple Devices)

If two devices share an SPI line (rare on modern SoCs but legacy PCI does
this), they OR into the same INTID. Linux walks `desc->action` (a linked list
of `irqaction`):

```c
for (action = desc->action; action; action = action->next) {
    res = action->handler(irq, action->dev_id);
    if (res == IRQ_HANDLED) handled = true;
}
```

Each driver must check **its own** device status register to know whether
*it* caused the IRQ. Drivers that didn't fire return `IRQ_NONE`. If all
return `IRQ_NONE` repeatedly, the kernel disables the line as "spurious"
(after ~100k unhandled).

### Re-Entrancy Protection

`handle_fasteoi_irq()` / `handle_edge_irq()` use:

- `raw_spin_lock(&desc->lock)` — serializes per-IRQ state machine
- `IRQD_IRQ_INPROGRESS` flag — set while running, blocks parallel entry
- `IRQS_PENDING` flag — set if HW re-fires while INPROGRESS; replayed at end

This guarantees: even with affinity changes mid-flight, a single virq's
handler never runs on two CPUs at once.

---

## Q3. Which Interrupt Has Higher Priority?

### GIC Priority Encoding

Every INTID has an **8-bit priority** in `GICD_IPRIORITYR<n>` (SPIs) or
`GICR_IPRIORITYR<n>` (SGIs/PPIs).

- Range: `0x00` (highest) — `0xFF` (lowest)
- Some bits may be RES0 depending on implementation
  (typically 4–5 implemented bits, so resolution is 0x10 or 0x08)

### Three Filtering Levels Per CPU

| Register | Effect |
|---|---|
| `ICC_PMR_EL1` (Priority Mask) | IRQs with priority ≥ PMR are blocked entirely |
| `ICC_RPR_EL1` (Running Priority) | IRQs with priority ≥ RPR can't preempt current handler |
| `ICC_BPR1_EL1` (Binary Point) | Splits priority into group/sub for preemption grouping |

#### `ICC_PMR_EL1` — Hard Filter

Linux normally sets `ICC_PMR_EL1 = 0xF0` (allow priorities 0x00–0xE0).

When using **pseudo-NMI** (`CONFIG_ARM64_PSEUDO_NMI`), Linux lowers PMR to
`GIC_PRIO_IRQOFF` (0xC0) instead of setting `PSTATE.I`. NMI-class IRQs are
configured with priority `0xA0` and remain unmasked even when "IRQs are off"
from `local_irq_disable()`. This is how arm64 implements true NMIs without
hardware NMI support.

#### `ICC_RPR_EL1` — Soft Preemption Boundary

Reading `ICC_IAR1_EL1` sets `RPR` to the acknowledged IRQ's priority. Writing
`ICC_EOIR1_EL1` restores it. A higher-priority IRQ arriving mid-handler will
re-assert nIRQ — *if* Linux re-enables `PSTATE.I`, which it normally does
**not** during the handler. So in practice, GIC preemption is unused under
Linux (the OS owns scheduling, not the GIC).

#### `ICC_BPR1_EL1` — Preemption Grouping

```
BPR=3 → priority[7:4] = group, priority[3:0] = sub-priority

  0x50 vs 0x60 → groups differ (5 vs 6) → 0x50 preempts 0x60
  0x52 vs 0x51 → same group (5)         → no preemption
```

Linux sets BPR aggressively (no preemption grouping) since it doesn't use
hardware preemption.

### Linux Priority Assignment Conventions

| INTID Class | Default Priority |
|---|---|
| Regular device IRQs | `GICD_INT_DEF_PRI` = 0xA0 |
| Pseudo-NMI IRQs | `GIC_PRIO_IRQON` = 0xC0 |
| Standard masked level | `GIC_PRIO_IRQOFF` = 0x60–0xA0 (depending on config) |

You rarely change priorities from drivers — Linux exposes no clean API for
it. The exceptions are perf NMI, RCU-stall detector NMI, and arm-smmu fault
IRQ.

### Tie-Breaking

When two pending IRQs have the *same* priority:

- GIC picks the **lowest INTID**
- This is deterministic hardware behavior — no fairness guarantee

---

## Q4. How Does the CPU Know Which Interrupt Occurred?

### Step 1 — Hardware Tells the CPU "An IRQ Happened" (No Number Yet!)

When the GICR asserts `nIRQ`, the CPU only learns *that* an IRQ is pending,
not *which* one. The CPU:

1. Saves `PC → ELR_EL1`, `PSTATE → SPSR_EL1`
2. Sets `PSTATE.I = 1`
3. Jumps to `VBAR_EL1 + 0x280` (from EL1) or `+ 0x480` (from EL0)

That's it. The CPU has no idea whether it was a UART, GPIO, timer, or IPI.

### Step 2 — Software Asks the GIC: Read `ICC_IAR1_EL1`

```c
u32 irqnr = read_sysreg_s(SYS_ICC_IAR1_EL1);
```

This single instruction (`mrs xN, ICC_IAR1_EL1`) atomically:

1. Returns the INTID of the highest-priority pending IRQ targeting this CPU.
2. Clears `GICD_ISPENDR[INTID]` (no longer pending).
3. Sets `GICD_ISACTIVER[INTID]` (now active).
4. Updates `ICC_RPR_EL1` to this INTID's priority.

If no IRQ is actually pending (race / spurious), returns `1023`
(`ICC_IAR1_EL1_SPURIOUS`).

### Step 3 — Translate HW INTID → Linux virq

The raw INTID is a hardware number. Linux uses **virtual IRQ numbers**
(virqs) so that different irqchips don't collide. The mapping lives in an
`irq_domain`:

```c
generic_handle_domain_irq(gic_data.domain, irqnr);
  → desc = irq_resolve_mapping(domain, hwirq);   // hash lookup
  → handle_irq_desc(desc);
    → desc->handle_irq(desc);                    // flow handler
```

`irq_domain` types:

| Type | Use |
|---|---|
| `LINEAR` | Small dense HW INTID space (GIC, GPIO bank) — array lookup |
| `TREE` | Sparse (e.g., LPI 8192+) — radix tree |
| `HIERARCHY` | Stacked irqchips (GPIO → GIC, MSI → ITS → GIC) — each level has own data |

### Step 4 — Flow Handler Picks a Strategy

Each `irq_desc` has a `handle_irq` callback set when the irqchip type was
configured:

| Flow Handler | When Used |
|---|---|
| `handle_fasteoi_irq` | GIC SPIs (level or edge); EOI is cheap |
| `handle_edge_irq` | Edge-triggered, supports re-trigger while running |
| `handle_level_irq` | Classic level (mask before, unmask after) |
| `handle_percpu_devid_irq` | PPIs (timer) — no locking, per-CPU dev_id |
| `handle_simple_irq` | Demuxer entry, no chip ack |

### Step 5 — For Multiplexed Controllers, Demux Again

A GPIO controller delivers **one** SPI for **32 pins**. The chained handler
reads the GPIO `INT_STATUS` register to learn which pin(s):

```c
status = readl(gc->base + GPIO_INT_STATUS);
for_each_set_bit(pin, &status, gc->ngpio) {
    int child_virq = irq_find_mapping(gc->irq.domain, pin);
    generic_handle_irq(child_virq);  // dive into per-pin handler
}
```

Same pattern: ITS does it for LPIs, PCIe MSI bridges do it for sub-vectors,
etc.

### End Result

After all the lookups, exactly one `irqaction->handler` runs with the right
`dev_id` — the driver knows which device, which queue, which line.

---

## Q5. How Do We Support Thousands of Interrupts?

### The Numbers

| Class | Range | Max Count |
|---|---|---|
| SGI | 0–15 | 16 |
| PPI | 16–31 | 16 per CPU |
| SPI | 32–1019 | 988 |
| Extended SPI (GICv3.1) | 4096–5119 | 1024 |
| LPI | 8192–2³² | ~4 billion |

A modern server with 100 NVMe drives × 64 queues = 6400 IRQs. Add network
adapters (multi-queue, per-CPU), GPUs, accelerators → easily 10K+ IRQs.
SPIs alone can't scale. The answer is **LPIs + ITS**.

### Mechanism 1 — Message-Signaled Interrupts (MSI / MSI-X)

PCIe devices don't have wires going to the GIC. Instead, they raise an IRQ
by **performing a memory write**:

```
Device writes (DeviceID, EventID) to GITS_TRANSLATER (MMIO @ 0x08080000)
```

The address goes to the **ITS**, the data carries `EventID`.

### Mechanism 2 — ITS Translation Pipeline

```
PCIe Write  ──►  ITS Translater
                    │
                    ├─► Device Table: DeviceID → ITT pointer
                    │
                    ├─► ITT (per-device):  EventID → (LPI INTID, CollectionID)
                    │
                    └─► Collection Table:  CollectionID → target GICR (CPU)
                            │
                            ▼
                       Target GICR: set bit in LPI Pending Table → nIRQ
```

The tables live in **kernel-allocated memory**, not on-chip registers, which
is exactly why LPI INTIDs can be 32-bit wide — there's no register file to
provision.

### Mechanism 3 — LPI Configuration in Memory

| Table | Pointer Register | Granularity |
|---|---|---|
| LPI Property Table (global) | `GICR_PROPBASER` | 1 byte per LPI: `[7:2]=priority, [1]=group, [0]=enable` |
| LPI Pending Table (per-CPU) | `GICR_PENDBASER` | 1 bit per LPI |
| Device Table | `GITS_BASER<n>` | 8 bytes per DeviceID |
| Collection Table | `GITS_BASER<n>` | 8/16 bytes per CollectionID |
| ITT (per-device) | Allocated by SW, mapped via `MAPD` cmd | Per-EventID |

Reconfiguring an LPI = writing a byte in DRAM + `INV` ITS command. No MMIO
register per LPI required.

### Mechanism 4 — ITS Command Queue

Configuration is asynchronous via an in-memory ring:

```
GITS_CBASER  → base address of command ring
GITS_CWRITER → producer pointer (SW writes here)
GITS_CREADR  → consumer pointer (HW updates)
```

Commands:

| Command | Purpose |
|---|---|
| `MAPD`  | Map a DeviceID → ITT |
| `MAPTI` | Map (DeviceID, EventID) → (LPI, Collection) |
| `MAPC`  | Map CollectionID → target GICR |
| `MOVI`  | Move an LPI to a new collection (affinity change) |
| `INV` / `INVALL` | Flush ITS caches |
| `SYNC`  | Wait for previous commands to take effect |

When you call `pci_alloc_irq_vectors()` for an NVMe device, the kernel
issues `MAPD` (once per device), then `MAPTI` for each requested vector.
Setting affinity later issues `MOVI`.

### Mechanism 5 — Linux Hierarchical irqdomain

For LPIs, the irqchip stack looks like:

```
PCI device  (msi_desc)
   │
   ▼
PCI-MSI domain          ── alloc/free vectors, write MSI capability
   │
   ▼
ITS-MSI domain          ── assign LPI INTID, talk to ITS
   │
   ▼
GICv3-ITS domain        ── per-CPU GICR mapping
```

Each layer is its own `irq_domain` with its own `alloc()` / `activate()` /
`set_affinity()`. `irq_domain_alloc_irqs()` walks down the stack and gives
the driver a virq that "just works".

### Putting It Together

A 64-queue NVMe device on an ARM64 server with GICv3+ITS:

1. NVMe driver: `pci_alloc_irq_vectors(pdev, 64, 64, PCI_IRQ_MSIX)`
2. Core MSI: allocates 64 `msi_desc`s, descends into ITS domain.
3. ITS domain: picks 64 LPI INTIDs (e.g., 8195–8258), allocates ITT for this
   device, issues `MAPD` + 64 × `MAPTI`.
4. GIC-ITS domain: maps each LPI to a per-CPU `GICR`. Default affinity
   pins queue *i* to CPU *i*.
5. Returns 64 virqs. Driver does `request_irq()` for each.
6. NVMe completion on queue 7: device writes MSI → ITS → LPI 8202 → CPU7's
   GICR → CPU7 reads `ICC_IAR1_EL1` → returns 8202 → Linux flow handler →
   driver's completion ISR runs.

No SPI was consumed. The same SoC can host thousands more such queues
because LPI INTIDs are essentially unlimited.

---

## Cross-Cutting Summary

| Question | GIC Mechanism | Linux Mechanism |
|---|---|---|
| Which CPU? | `GICD_IROUTER` / GICR / ITS collections | `irq_set_affinity`, sysfs, irqbalance, managed IRQs |
| Concurrent IRQs? | Per-CPU arbitration on pending+priority | `gic_handle_irq` loop, `desc->lock`, IRQ stack |
| Higher priority? | `GICD_IPRIORITYR`, `ICC_PMR_EL1`, `ICC_RPR_EL1` | `GICD_INT_DEF_PRI` (0xA0), pseudo-NMI scheme |
| Which IRQ fired? | Read `ICC_IAR1_EL1` returns INTID | `irq_domain` resolves → flow handler → demux |
| Thousands of IRQs? | LPIs (32-bit ID space), ITS, in-memory tables | Hierarchical irqdomains, MSI core, `pci_alloc_irq_vectors` |

These five answers, working together, are what make modern ARM64 servers
viable for thousand-core, thousand-device workloads.
