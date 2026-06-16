# Memory Ordering & Barriers

## 1. Why Memory Ordering Matters

ARM has a **weakly-ordered memory model**. The hardware is free to reorder memory
accesses for performance, as long as single-threaded behavior is preserved.
But in multi-threaded programs, this reordering can cause bugs.

```
Example problem:

  Core 0 (Producer):           Core 1 (Consumer):
  ─────────────────            ─────────────────
  data = 42;       // Store 1  while (flag == 0);  // Load A
  flag = 1;        // Store 2  print(data);        // Load B

  Expected: Consumer prints 42
  
  On strongly-ordered CPU (x86): Always correct
  On weakly-ordered CPU (ARM):   Might print GARBAGE!
  
  Why? ARM hardware might:
    • Reorder Store 1 and Store 2 (data written after flag)
    • Consumer sees flag=1 but data hasn't been written yet
    
  Solution: Memory barriers between Store 1 and Store 2
```

---

## 2. ARM Memory Ordering Rules

### What ARM Guarantees (Without Barriers)

```
For a SINGLE core:
  ✓ Dependencies are respected
    LDR X0, [X1]          // Load A
    ADD X2, X0, #1        // Uses X0 → must wait for Load A
    → Hardware cannot reorder these

  ✓ Overlapping addresses maintain order
    STR X0, [X1]          // Store to address A
    LDR X2, [X1]          // Load from address A  
    → Load sees the stored value

  ✓ Control dependencies to stores
    CBZ X0, skip
    STR X1, [X2]          // Only if X0 ≠ 0
    → Store won't happen speculatively

For MULTIPLE cores (without barriers):
  ✗ Stores can appear in different order to other cores
  ✗ A load can see a store from another core before other observers
  ✗ Independent loads can be reordered
  ✗ Independent stores can be reordered
```

### What Can Be Reordered?

```
┌──────────────────────────────────────────────────────────────┐
│                     Can be reordered?                         │
│                                                               │
│  Operation pair          Single core    Multi-core            │
│  ──────────────────      ───────────    ──────────            │
│  Load → Load (diff addr)    YES*          YES                │
│  Load → Store (diff addr)   YES*          YES                │
│  Store → Load (diff addr)   YES           YES                │
│  Store → Store (diff addr)  YES           YES                │
│                                                               │
│  Load → Load (same addr)    NO            NO                 │
│  Store → Store (same addr)  NO            NO                 │
│  Load → dependent op        NO            NO                 │
│                                                               │
│  * = may still appear ordered due to register dependency      │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. Barrier Instructions

### DMB — Data Memory Barrier

```
DMB ensures ordering between memory accesses:

  All memory accesses BEFORE DMB are observed before
  any memory access AFTER DMB.

  STR X0, [X1]        // Store A
  DMB ISH              // Barrier
  STR X2, [X3]        // Store B
  → All cores see Store A before Store B

Variants (shareability domain):
  DMB SY     — Full System (all observers: CPU, GPU, DMA, etc.)
  DMB ISH    — Inner Shareable (all CPUs in same domain)
  DMB OSH    — Outer Shareable (across clusters)
  DMB NSH    — Non-Shareable (local core only)

Access type restriction:
  DMB ISHLD  — Only orders Loads before barrier against Loads/Stores after
  DMB ISHST  — Only orders Stores before barrier against Stores after
  DMB ISH    — Orders all accesses before against all accesses after
```

### DSB — Data Synchronization Barrier

```
DSB is STRONGER than DMB:
  Like DMB, plus:
  • Blocks execution of ANY instruction (not just memory ops)
    until all prior memory accesses complete
  • Required before cache/TLB maintenance to ensure visibility

  Example: TLB invalidation
  STR X0, [X1]         // Modify page table entry
  DSB ISH              // Wait for store to complete
  TLBI VAE1IS, X2      // Invalidate TLB (only safe after DSB)
  DSB ISH              // Wait for TLBI to complete on all cores
  ISB                  // Flush pipeline
```

### ISB — Instruction Synchronization Barrier

```
ISB flushes the pipeline:
  • All instructions before ISB must complete
  • Pipeline is emptied
  • Instructions after ISB are re-fetched
  • System register changes take effect

  Required after:
  • MSR to SCTLR (changing MMU/cache config)
  • MSR to TCR, TTBR (changing page tables)
  • MSR to VBAR (changing exception vectors)
  • Writing to instruction memory (self-modifying code)

  Example: Changing vector table
  MSR VBAR_EL1, X0     // Set new vector base
  ISB                   // Pipeline sees new value for next exception
```

---

## 4. Acquire and Release Semantics

Instead of explicit barriers, ARM provides **acquire/release** instructions:

```
┌──────────────────────────────────────────────────────────────────┐
│  Concept       Instruction    Ordering guarantee                 │
├──────────────────────────────────────────────────────────────────┤
│  Acquire       LDAR           All loads/stores AFTER LDAR are   │
│                                 observed after the LDAR itself   │
│                                → "Acquire the lock, then access" │
│                                                                   │
│  Release       STLR           All loads/stores BEFORE STLR are  │
│                                 observed before the STLR itself  │
│                                → "Finish work, then release lock"│
│                                                                   │
│  Acquire+      LDAPR          Weaker acquire (load-acquire of   │
│  Release       (ARMv8.3)       prior release stores)             │
└──────────────────────────────────────────────────────────────────┘

Producer-Consumer with Acquire/Release:

  Core 0 (Producer):           Core 1 (Consumer):
  ─────────────────            ─────────────────
  STR X0, [data_ptr]          loop:
  STLR X1, [flag_ptr]  ←REL    LDAR X2, [flag_ptr]  ←ACQ
  // Release: data is           // Acquire: data is
  // visible before flag         // visible after flag
                                 CBZ X2, loop
                                 LDR X3, [data_ptr]  // Guaranteed to see 42!
```

### Exclusive + Acquire/Release (Spinlock)

```
Lock acquire:
  lock:
    LDAXR W0, [X1]        // Load-Acquire Exclusive (LDAR + exclusive)
    CBNZ  W0, lock         // If locked, retry
    STXR  W2, W3, [X1]    // Store Exclusive (try to acquire)
    CBNZ  W2, lock         // If failed, retry
    // Lock acquired — all subsequent accesses ordered after this

Lock release:
    STLR  WZR, [X1]       // Store-Release (STLR: all prior accesses 
                           //  are visible before lock value = 0)

With LSE atomics (ARMv8.1):
  Lock acquire:
    MOV W0, #1
    SWPA W0, W0, [X1]     // Swap-Acquire: atomically set lock
    CBNZ W0, spin          // If was already locked, spin

  Lock release:
    STLR WZR, [X1]        // Store-Release zero
```

---

## 5. One-Way Barriers (Load-Acquire / Store-Release) Visualized

```
                Memory operations timeline
                ─────────────────────────────
  
  LDAR (Load-Acquire):
  ──────────────────────────────────────────
    Earlier loads/stores  │  CAN move before LDAR
                          │
         ► LDAR ◄─────── │ ─── BARRIER ───
                          │
    Later loads/stores    │  CANNOT move before LDAR
  ──────────────────────────────────────────
  
  (One-way fence: blocks downward movement only)
  
  
  STLR (Store-Release):
  ──────────────────────────────────────────
    Earlier loads/stores  │  CANNOT move after STLR
                          │
         ► STLR ◄─────── │ ─── BARRIER ───
                          │
    Later loads/stores    │  CAN move after STLR
  ──────────────────────────────────────────
  
  (One-way fence: blocks upward movement only)

  
  DMB (Full Barrier):
  ──────────────────────────────────────────
    Earlier loads/stores  │  CANNOT move after DMB
                          │
          ► DMB ◄──────── │ ─── BARRIER ───
                          │
    Later loads/stores    │  CANNOT move before DMB
  ──────────────────────────────────────────
  
  (Two-way fence: blocks both directions)
```

---

## 6. Practical Barrier Usage

```
┌──────────────────────────────────────────────────────────────┐
│  Scenario                         Barrier needed              │
├──────────────────────────────────────────────────────────────┤
│  Spinlock acquire                  LDAXR / SWPA (acquire)    │
│  Spinlock release                  STLR (release)            │
│  Write data + set flag             STLR for flag (release)   │
│  Read flag + read data             LDAR for flag (acquire)   │
│  DMA buffer setup                  DMB SY + DSB SY           │
│  MMIO register access              DMB (between accesses)    │
│  Page table update                 DSB ISH + TLBI + DSB + ISB│
│  System register change            ISB                        │
│  Self-modifying code               DC CVAU + DSB + IC + DSB + ISB │
│  IPI / cross-core notification     SEV + DMB ISH              │
│  Atomic counter                    LDADD (LSE, relaxed ok)   │
└──────────────────────────────────────────────────────────────┘
```

---

## 7. C/C++ Memory Model Mapping

```
C++ memory_order    →    ARM instructions
───────────────────      ────────────────────────
memory_order_relaxed     LDR / STR (no barrier)
memory_order_consume     LDR + dependent ops (address dependency)
memory_order_acquire     LDAR
memory_order_release     STLR
memory_order_acq_rel     LDAXR/STLXR or DMB ISH between
memory_order_seq_cst     LDAR / STLR (full sequential consistency)

Example: std::atomic<int> x;
  x.store(42, std::memory_order_release);  → STLR W0, [X1]
  int val = x.load(std::memory_order_acquire); → LDAR W0, [X1]
```

---

Next: Back to [Memory Subsystem Overview](./README.md) | Continue to [Cache Subsystem →](../03_Cache_Subsystem/)
