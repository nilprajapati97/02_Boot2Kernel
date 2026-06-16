# Cache Coherency

## 1. What is Cache Coherency?

In a multi-core system, each core has its own private L1/L2 caches. When multiple
cores access the **same memory address**, their caches can have different values.
**Cache coherency** ensures all cores see a consistent view of memory.

```
The Problem:

  Core 0                           Core 1
  ┌──────────────┐                ┌──────────────┐
  │  L1 Cache    │                │  L1 Cache    │
  │ addr 0x1000: │                │ addr 0x1000: │
  │  value = 42  │                │  value = 42  │
  └──────┬───────┘                └──────┬───────┘
         │                               │
         └───────────┬───────────────────┘
                     │
              ┌──────┴──────┐
              │ Main Memory  │
              │ 0x1000 = 42  │
              └─────────────┘

  Core 0 writes: 0x1000 = 99 (only updates its L1 cache!)

  Core 0                           Core 1
  ┌──────────────┐                ┌──────────────┐
  │ addr 0x1000: │                │ addr 0x1000: │
  │  value = 99  │  ← Updated    │  value = 42  │  ← STALE!
  └──────────────┘                └──────────────┘

  Without coherency: Core 1 reads 42 (WRONG!)
  With coherency:    Core 1 reads 99 (CORRECT!)
```

---

## 2. Coherency in ARM Architecture

ARM uses a hardware **snoop-based coherency protocol** to keep caches consistent.
The protocol is implemented in the interconnect (CCI, CCN, DSU, CMN).

### How Snooping Works

```
┌─────────────────────────────────────────────────────────────────┐
│                     Snoop-Based Coherency                       │
│                                                                  │
│  Core 0 wants to write to address X:                            │
│                                                                  │
│  1. Core 0 sends "write intent" to interconnect                 │
│                                                                  │
│  2. Interconnect SNOOPS all other cores:                        │
│     "Does anyone have address X in their cache?"                │
│                                                                  │
│     ┌────────┐    Snoop     ┌────────┐    Snoop     ┌────────┐ │
│     │ Core 0 │ ────────────▶│ Core 1 │ ────────────▶│ Core 2 │ │
│     │(writer)│              │(reader)│              │(none)  │ │
│     └────────┘              └────┬───┘              └────────┘ │
│                                  │                              │
│  3. Core 1 responds:            │                              │
│     "Yes, I have it (clean/dirty)"                              │
│     → If dirty: supply the data directly (cache-to-cache)      │
│     → Invalidate or downgrade Core 1's copy                    │
│                                                                  │
│  4. Core 0 gets exclusive ownership → writes safely             │
│                                                                  │
│  5. Core 2 responds: "I don't have it" → no action             │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Snoop Control Unit (SCU)

In older ARM designs (Cortex-A9, A15, A17), the **SCU** manages coherency
within a cluster:

```
┌─────────────────────────────────────────────────────┐
│                    CPU Cluster                       │
│                                                       │
│  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐    │
│  │ Core 0 │  │ Core 1 │  │ Core 2 │  │ Core 3 │    │
│  │ L1I+D  │  │ L1I+D  │  │ L1I+D  │  │ L1I+D  │    │
│  └───┬────┘  └───┬────┘  └───┬────┘  └───┬────┘    │
│      │           │           │           │          │
│  ┌───┴───────────┴───────────┴───────────┴───┐     │
│  │           Snoop Control Unit (SCU)          │     │
│  │                                              │     │
│  │  • Tracks which core has which cache line   │     │
│  │  • Intercepts snoop requests                 │     │
│  │  • Manages data forwarding between L1s      │     │
│  │  • Duplicate tag directory (snoop filter)    │     │
│  └──────────────────┬──────────────────────────┘     │
│                      │                                │
│  ┌──────────────────┴──────────────────────────┐     │
│  │              Shared L2 Cache                  │     │
│  └──────────────────┬──────────────────────────┘     │
│                      │                                │
└──────────────────────┼────────────────────────────────┘
                       │
              To Interconnect (CCI/CCN/CMN)
```

### DynamIQ Shared Unit (DSU) — Modern ARM

In newer ARM designs (Cortex-A55/A75+ onwards), the **DSU** replaces the SCU:

```
┌─────────────────────────────────────────────────────┐
│             DynamIQ Shared Unit (DSU)                │
│                                                       │
│  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐    │
│  │ A55    │  │ A55    │  │ A78    │  │ A78    │    │
│  │(Little)│  │(Little)│  │(big)  │  │(big)  │    │
│  │L1+L2  │  │L1+L2  │  │L1+L2  │  │L1+L2  │    │
│  └───┬────┘  └───┬────┘  └───┬────┘  └───┬────┘    │
│      │           │           │           │          │
│  ┌───┴───────────┴───────────┴───────────┴───┐     │
│  │              DSU Logic                      │     │
│  │  • L3 Cache (shared, 1-8 MB)               │     │
│  │  • Snoop filter (tracks all L1/L2 lines)   │     │
│  │  • Coherency manager                        │     │
│  │  • Power management (per-core gating)       │     │
│  │  • Supports heterogeneous cores (big.LITTLE)│     │
│  └──────────────────┬──────────────────────────┘     │
│                      │                                │
│                      │ ACE / CHI interface             │
└──────────────────────┼────────────────────────────────┘
                       │
              To CMN / Interconnect
```

---

## 4. Coherency Protocols Used in ARM

ARM systems use different coherency protocols at different levels:

```
┌────────────────────────────────────────────────────────────────┐
│  Level              Protocol         Description               │
├────────────────────────────────────────────────────────────────┤
│  Within cluster     MOESI / MESI     Snoop-based, HW managed │
│  (L1 ↔ L1)         (via SCU/DSU)                              │
│                                                                │
│  Between clusters   ACE protocol     AMBA ACE (AXI Coherency  │
│  (cluster ↔ cluster) or CHI         Extensions) or AMBA CHI   │
│                                       over CCI/CCN/CMN         │
│                                                                │
│  System level       CHI (newer)      Coherent Hub Interface   │
│  (CPU ↔ GPU ↔ IO)   ACE-Lite        Read-only coherent (GPU)  │
│                      ACE              Full coherent (CPU)      │
└────────────────────────────────────────────────────────────────┘
```

---

## 5. Snoop Filter / Directory

A **snoop filter** (or directory) avoids broadcasting snoops to ALL cores:

```
Without snoop filter (broadcast):
  Core 0 writes addr X
  → Snoop sent to Core 1, Core 2, Core 3, Core 4, Core 5...
  → Even if only Core 2 has the line! (wasted bandwidth)

With snoop filter (directory-based):
  ┌─────────────────────────────────────┐
  │  Snoop Filter / Directory           │
  │                                      │
  │  Address │ Present in               │
  │  ────────┼──────────────            │
  │  0x1000  │ Core 0, Core 2           │
  │  0x2000  │ Core 1                    │
  │  0x3000  │ Core 0, Core 1, Core 3   │
  └─────────────────────────────────────┘

  Core 0 writes addr 0x1000
  → Directory lookup: only Core 2 has it
  → Snoop sent ONLY to Core 2 (saves bandwidth!)
  → Scales much better for many-core systems
```

---

## 6. Cache Coherency Point (PoC) and Point of Unification (PoU)

ARM defines specific **points** in the memory hierarchy for coherency and maintenance:

```
┌──────────────────────────────────────────────────────────────────┐
│                                                                   │
│  Core                                                            │
│  ┌──────────┐                                                    │
│  │ L1 I$    │──┐                                                 │
│  └──────────┘  │                                                 │
│                ├── PoU (Point of Unification)                    │
│  ┌──────────┐  │   = Where I$ and D$ "see the same data"        │
│  │ L1 D$    │──┘   = Typically L2 or L1 (if coherent I$/D$)     │
│  └──────────┘                                                    │
│       │                                                          │
│  ┌────▼─────┐                                                    │
│  │  L2 $    │  ← PoU for many ARM cores                         │
│  └────┬─────┘                                                    │
│       │                                                          │
│  ┌────▼─────┐                                                    │
│  │  L3 $    │                                                    │
│  └────┬─────┘                                                    │
│       │                                                          │
│  ┌────▼─────────────┐                                            │
│  │  Main Memory      │  ← PoC (Point of Coherency)              │
│  │  (DRAM)           │     = Where ALL observers agree on data   │
│  └──────────────────┘     = All caches are coherent here         │
│                                                                   │
│  GPU, DMA, other masters can also see memory at PoC              │
└──────────────────────────────────────────────────────────────────┘

PoC: Clean/Invalidate to PoC → data goes to main memory (DMA-safe)
PoU: Clean/Invalidate to PoU → I$/D$ coherent (self-modifying code)
```

---

## 7. Shareable vs Non-Shareable

```
Shareability affects coherency scope:

Non-Shareable (NSH):
  • Only the local core's cache participates
  • No snooping, no coherency with other cores
  • Used for: core-private data, single-threaded buffers

Inner Shareable (ISH):
  • All cores in the inner shareable domain
  • Hardware coherency maintained automatically
  • Most common for: all shared memory (Linux default)

Outer Shareable (OSH):
  • All observers in the outer shareable domain
  • Includes GPU, DMA, other bus masters
  • Used for: DMA buffers, GPU shared memory

Page table SH bits control this per-page:
  SH = 00 → Non-Shareable
  SH = 10 → Outer Shareable
  SH = 11 → Inner Shareable
```

---

Next: [MESI/MOESI Protocols →](./03_MESI_MOESI_Protocols.md)
