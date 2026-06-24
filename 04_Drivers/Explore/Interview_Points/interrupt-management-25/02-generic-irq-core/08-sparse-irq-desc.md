# Q8 — Sparse IRQs and irq_desc Management

> **Subsystem:** Generic IRQ Core · **Files:** `kernel/irq/irqdesc.c`, `include/linux/irqdesc.h`, `kernel/irq/internals.h`
> **Interviewer is really probing:** Do you understand **why IRQ descriptors are allocated dynamically**
> (SPARSE_IRQ), how `virq` numbers are managed (radix tree / allocator), and the relationship to MSI/hotplug?

---

## TL;DR Cheat Sheet

- **`CONFIG_SPARSE_IRQ`** makes `irq_desc`s **dynamically allocated** and looked up via a **radix tree**
  (keyed by `virq`), instead of a giant static `irq_desc[NR_IRQS]` array. Default on modern kernels.
- **Why:** modern systems have **huge, sparse** IRQ spaces — thousands of potential **MSI/MSI-X** vectors
  (Q4), GPIO lines, hot-pluggable PCIe devices — most **unused**. A static array sized for the maximum would
  waste memory and cap the count; sparse allocates **only the descriptors actually in use**.
- **`virq` allocation:** `irq_alloc_descs(irq, from, cnt, node)` reserves a contiguous block of Linux IRQ
  numbers and their `irq_desc`s; `irq_domain` (Q3) calls this when mapping a hwirq. `irq_free_descs` releases
  them (device removal, MSI teardown).
- **Lookup:** `irq_to_desc(virq)` walks the radix tree (sparse) or indexes the array (non-sparse).
- **Dynamic IRQs:** drivers/MSI/`irq_domain` allocate IRQs **at runtime** (`irq_domain_alloc_irqs`,
  `pci_alloc_irq_vectors`, Q4) — descriptors are created on demand and freed on release, supporting
  **hotplug** and **per-device** vector counts.
- **NUMA-aware:** descriptors are allocated on the **node** of the owning device for locality.

---

## The Question

> What is `SPARSE_IRQ` and why does it exist? How are `irq_desc`s and Linux IRQ numbers (`virq`) allocated and
> looked up, and how does this support MSI and hotplug?

What they want: the **dynamic descriptor model** (radix tree vs static array), the **virq allocator**, and
**why** modern IRQ counts (MSI-X, GPIO, hotplug) made the old static array untenable.

---

## Why sparse IRQs exist

Historically Linux had a **static global array**: `struct irq_desc irq_desc[NR_IRQS]`, indexed directly by
IRQ number. That worked when systems had a **small, dense, fixed** set of IRQs (the 16 legacy PIC lines, a
handful of IO-APIC inputs). Two trends broke it:

- **MSI/MSI-X exploded the IRQ count (Q4):** a single multi-queue NIC/NVMe/GPU can request **dozens to
  thousands** of vectors; a busy server has **many** such devices. Sizing `NR_IRQS` for the worst case means a
  **massive, mostly-empty** static array consuming memory for descriptors that are never used. And any fixed
  cap eventually becomes a **limit**.
- **The IRQ space became sparse and dynamic:** GPIO controllers, PMICs, hot-pluggable PCIe, and per-device
  MSI domains create IRQs **at runtime** and free them on removal. A static array can't grow/shrink and wastes
  space on gaps.

**`CONFIG_SPARSE_IRQ`** solves this by making `irq_desc`s **dynamically allocated** and stored in a **radix
tree** keyed by `virq`. Now:
- you allocate a descriptor **only when an IRQ is actually mapped** (by `irq_domain`, Q3),
- the `virq` space can be **large and sparse** without wasting memory,
- IRQs can be **created/destroyed at runtime** (hotplug, MSI alloc/free),
- descriptors can be placed **NUMA-locally** to their device.

The senior framing: sparse IRQs are the **infrastructure that makes MSI-X multi-queue and PCIe hotplug
feasible** — they decouple "the IRQ number space" from "memory for descriptors," allocating on demand. It ties
directly to `irq_domain` (Q3, which drives the allocation) and MSI (Q4, the biggest consumer).

---

## When descriptors are allocated/freed

| Event | Action |
|-------|--------|
| irqchip driver maps a hwirq (Q3) | `irq_domain` → `irq_alloc_descs` → create `irq_desc`(s) |
| MSI/MSI-X allocation (Q4) | `pci_alloc_irq_vectors` → hierarchical `irq_domain_alloc_irqs` → descs |
| GPIO/PMIC line first used | mapped on demand via the controller's domain |
| device removal / MSI teardown | `irq_free_descs` releases descriptors and virq range |
| `/proc/interrupts` read (Q21) | iterates allocated descriptors (radix tree) |
| CPU/PCIe hotplug | descriptors created/destroyed dynamically |

---

## Where in the kernel

```
kernel/irq/irqdesc.c       <- alloc_desc/free_desc, irq_to_desc, irq_alloc_descs/free_descs,
                              the radix tree (irq_desc_tree) + allocated_irqs bitmap
include/linux/irqdesc.h     <- struct irq_desc, irq_to_desc accessors
kernel/irq/irqdomain.c      <- domain mapping calls irq_alloc_descs / __irq_domain_alloc_irqs (Q3)
kernel/irq/internals.h      <- internal helpers
Config: CONFIG_SPARSE_IRQ, NR_IRQS / nr_irqs
```

---

## How it works — mechanics

### 1. Static array vs sparse radix tree

```
NON-SPARSE (legacy):
   struct irq_desc irq_desc[NR_IRQS];     // fixed array; irq_to_desc(i) = &irq_desc[i]
   - simple O(1) index, but sized for the MAX -> wastes memory, hard cap

SPARSE (CONFIG_SPARSE_IRQ, default):
   radix tree:  virq -> struct irq_desc*  (irq_desc_tree)
   bitmap:      allocated_irqs            (which virqs are in use)
   - irq_to_desc(virq) = radix_tree_lookup(&irq_desc_tree, virq)
   - descriptors allocated on demand, freed on release; sparse & dynamic
```

### 2. Allocating a virq + descriptor

```c
/* Reserve `cnt` contiguous virqs starting at/after `from`, on NUMA node `node`. */
int irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node);
/* internally: find a free range in allocated_irqs bitmap, alloc_desc() each,
   insert into the radix tree, return the base virq */
```
`irq_domain` (Q3) calls this when it needs to map hwirqs to virqs (e.g. on `irq_create_mapping` or
`irq_domain_alloc_irqs` for MSI). Each `alloc_desc()` allocates an `irq_desc` (with its per-CPU `kstat_irqs`,
Q21), sets defaults, and places it **on the device's NUMA node** for locality. The returned **virq** is what
drivers use (`request_irq`, Q9).

### 3. Lookup at interrupt time

When an interrupt fires, the controller code maps hwirq → virq (`irq_find_mapping`, Q3) and calls
`generic_handle_irq(virq)` → `irq_to_desc(virq)` → the radix-tree lookup → the `irq_desc` whose flow handler
(Q7) runs. The radix tree lookup is fast and cache-friendly for the sparse space.

### 4. Freeing

```c
void irq_free_descs(unsigned int irq, unsigned int cnt);
```
On device removal or MSI teardown (Q4), the domain frees the virq range: clears the `allocated_irqs` bits,
removes descriptors from the radix tree, frees the memory. This **reclaims** the IRQ numbers for reuse —
essential for **hotplug** and for not leaking descriptors across device bind/unbind cycles.

### 5. `NR_IRQS` / `nr_irqs` and dynamic growth

With sparse IRQ, `nr_irqs` is a **soft** upper bound that can grow as needed (within limits); the legacy hard
`NR_IRQS` array sizing is gone. The number of **actually allocated** descriptors tracks real usage, visible in
`/proc/interrupts` (Q21). MSI domains can allocate large contiguous virq blocks (one per device's vector set).

### 6. Relationship to the rest

- **`irq_domain` (Q3)** is the *caller* — it decides hwirq→virq and drives allocation.
- **MSI (Q4)** is the *big consumer* — `pci_alloc_irq_vectors` ultimately allocates sparse descriptors.
- **`irq_desc` (Q6)** is *what gets allocated* — the per-IRQ hub with flow handler + irqaction chain.
- **hotplug/PM** rely on dynamic alloc/free to add/remove device IRQs at runtime.

---

## Diagrams

### Sparse descriptor storage

```mermaid
flowchart TD
    DOM["irq_domain maps hwirq (Q3)"] --> ALLOC["irq_alloc_descs(from, cnt, node)"]
    ALLOC --> BITMAP[reserve range in allocated_irqs bitmap]
    ALLOC --> DESC[alloc_desc() per virq: irq_desc on device's NUMA node]
    DESC --> TREE["insert into radix tree (virq -> irq_desc*)"]
    INT["interrupt: irq_to_desc(virq)"] --> TREE
    FREE["device removed -> irq_free_descs"] --> TREE
```

### Static vs sparse

```
static:   [desc0][desc1][desc2]...[descNR_IRQS-1]   <- huge fixed array, mostly empty for MSI
sparse:   radix tree: {12->desc, 4096->desc, 9000->desc, ...}  <- only what's used, dynamic
```

---

## Annotated C

```c
/* Lookup (sparse: radix tree; non-sparse: array index). */
struct irq_desc *irq_to_desc(unsigned int irq);

/* Allocate a contiguous block of virqs + their descriptors, NUMA-local. */
int irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node);
#define irq_alloc_desc(node)            irq_alloc_descs(-1, 0, 1, node)
#define irq_alloc_descs_from(from, cnt, node) irq_alloc_descs(-1, from, cnt, node)

/* Free them (device removal / MSI teardown). */
void irq_free_descs(unsigned int irq, unsigned int cnt);

/* Internals (kernel/irq/irqdesc.c): the radix tree + bitmap. */
static RADIX_TREE(irq_desc_tree, GFP_KERNEL);   /* virq -> irq_desc* */
/* static DECLARE_BITMAP(allocated_irqs, IRQ_BITMAP_BITS); */

/* alloc_desc() sets up per-CPU stats, default flow handler, NUMA node, lock. */
```

> Senior nuance: sparse IRQ is **"allocate descriptors on demand, look them up in a radix tree."** It exists
> because **MSI-X** (Q4) and dynamic/hotplug IRQs blew up the IRQ-number space — a static `irq_desc[NR_IRQS]`
> array would waste huge memory and impose a hard cap. The **`irq_domain`** (Q3) drives allocation; descriptors
> are **NUMA-local**; freeing reclaims numbers for hotplug. Connect it to Q3/Q4/Q6 and you've shown the whole
> picture.

---

## Company Angle

- **AMD/Intel/NVIDIA (MSI-heavy):** sparse IRQ is what makes **thousands of MSI-X vectors** (Q4) across many
  devices memory-feasible; per-device dynamic alloc/free on bind/unbind; vector/desc NUMA locality.
- **Qualcomm/ARM (SoC):** many GPIO/PMIC/SoC interrupts mapped on demand via domains (Q3); dynamic IRQs for
  hot-added peripherals.
- **Google (servers):** PCIe hotplug, large IRQ counts, `/proc/interrupts` at scale (Q21), descriptor
  locality on NUMA.
- **All:** understanding that **virq numbers are allocated, not fixed**, and looked up via a radix tree.

---

## War Story

*"On a server with **many MSI-X-heavy** NVMe/NIC devices, we hit a kernel built **without
`CONFIG_SPARSE_IRQ`** (an old vendor config) where `NR_IRQS` was too small — device probe failed allocating
IRQs once the cumulative MSI-X vector count exceeded the **static array** size, and bumping `NR_IRQS` just
ballooned a mostly-empty array's memory. Switching to a kernel with **`SPARSE_IRQ`** (the modern default)
fixed it: descriptors became **dynamically allocated** in a radix tree, so the large, sparse vector space
cost only what was **actually used**, and there was no hard `NR_IRQS` wall. We also confirmed descriptors were
allocated on each device's **NUMA node** (`irq_alloc_descs(..., node)`) for locality, which helped interrupt
handling latency. The interviewer's follow-up — *'why not just make the static array bigger?'* — let me
explain that an array sized for thousands of potential MSI vectors **per device × many devices** wastes
enormous memory on **unused** descriptors and still imposes a cap; sparse allocation tracks **real usage** and
supports **hotplug** (free on unbind) — which a fixed array can't."*

---

## Interviewer Follow-ups

1. **What is `SPARSE_IRQ`?** Dynamically-allocated `irq_desc`s stored in a **radix tree** keyed by `virq`,
   replacing the static `irq_desc[NR_IRQS]` array.

2. **Why is it needed?** Modern IRQ spaces are **huge and sparse** (MSI-X vectors, GPIO, hotplug); a static
   array wastes memory and caps the count.

3. **How are virqs allocated?** `irq_alloc_descs(irq, from, cnt, node)` reserves a contiguous virq block +
   descriptors (NUMA-local); called by `irq_domain` (Q3).

4. **How is a descriptor looked up?** `irq_to_desc(virq)` — radix-tree lookup (sparse) or array index
   (non-sparse).

5. **Who drives allocation?** `irq_domain` mapping (Q3); the biggest consumer is **MSI** (`pci_alloc_irq_
   vectors`, Q4).

6. **How does freeing work?** `irq_free_descs` clears the bitmap, removes from the tree, frees memory — on
   device removal/MSI teardown; reclaims numbers for hotplug.

7. **Are descriptors NUMA-aware?** Yes — allocated on the owning device's node for locality.

8. **What replaced `NR_IRQS` as a hard limit?** `nr_irqs` is a soft, growable bound under sparse IRQ; the
   static array sizing is gone.

9. **How does this relate to `irq_desc`/`irq_domain`/MSI?** Sparse IRQ is the *storage/allocation* of
   `irq_desc` (Q6); `irq_domain` (Q3) decides the mapping; MSI (Q4) is the major dynamic consumer.

---

## 30-Minute Talk Track

| Min | Cover |
|-----|-------|
| 0–4 | Legacy static irq_desc[NR_IRQS]; why it broke (MSI-X explosion, sparse/dynamic IRQs) |
| 4–9 | SPARSE_IRQ: radix tree (virq→desc) + allocated_irqs bitmap; on-demand allocation |
| 9–13 | irq_alloc_descs: reserve virq block + descriptors, NUMA-local; driven by irq_domain (Q3) |
| 13–17 | Lookup at interrupt time: irq_to_desc(virq) → flow handler (Q7) |
| 17–20 | Freeing: irq_free_descs on removal/MSI teardown; hotplug support |
| 20–24 | Relationship to MSI (Q4 big consumer), irq_domain (Q3 driver), irq_desc (Q6 content) |
| 24–27 | nr_irqs soft bound; /proc/interrupts iterating allocated descs (Q21) |
| 27–30 | War story (NR_IRQS wall → sparse) + "track real usage, support hotplug" |
