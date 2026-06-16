# Memory Ordering: Complete Interview Reference

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Memory Model in 3 Minutes

```
ARM64 memory model: Weakly Ordered (WO)
  CPU can reorder all loads and stores except:
    1. Same-address accesses: always ordered
    2. Data-dependent loads: if Load B address depends on Load A value → ordered
  
  All other combinations: may be reordered by CPU and store buffer

ARM64 guarantees summary:
  ✓ LOAD → LOAD(same addr): ordered
  ✓ LOAD A → LOAD B: ordered IF B's address depends on A's VALUE
  ✓ Single-copy atomic: stores become visible to ALL CPUs simultaneously
  ✗ LOAD → LOAD(different addr, no dependency): NOT ordered
  ✗ STORE → STORE(different addr): NOT ordered
  ✗ STORE → LOAD: NOT ordered (requires full DMB)

Barriers available:
  DMB ISH   (smp_mb)  = full bidirectional, CPU scope
  DMB ISHLD (smp_rmb) = load-load, CPU scope
  DMB ISHST (smp_wmb) = store-store, CPU scope
  DSB ISH             = pipeline stall + memory complete, CPU scope
  DSB SY    (mb)      = full system barrier
  ISB                 = instruction pipeline flush
  LDAR (smp_load_acquire)  = one-way acquire fence
  STLR (smp_store_release) = one-way release fence
  LDAXR/STXR = LL/SC exclusive (with acquire/release variants)
  CASAL      = compare-and-swap, acquire+release (LSE)
```

---

## 2. Critical Pattern Reference

```
Pattern A: Producer-Consumer (message passing)
  Producer:
    WRITE_ONCE(data, val);
    smp_store_release(&ready, 1);  // STLR
  Consumer:
    while (!smp_load_acquire(&ready));  // LDAR
    val = READ_ONCE(data);
  
Pattern B: Mutual exclusion (Dekker-like)
  Thread A:
    WRITE_ONCE(flag_a, 1);
    smp_mb();              // FULL barrier (Store-Load ordering needed!)
    if (!READ_ONCE(flag_b)) enter_cs();
  Thread B:
    WRITE_ONCE(flag_b, 1);
    smp_mb();
    if (!READ_ONCE(flag_a)) enter_cs();

Pattern C: Seqlock (fast read-path)
  Writer:
    spin_lock(&lock);
    smp_wmb(); seq++;      // odd = write in progress
    update_data();
    smp_wmb(); seq++;      // even = write done
    spin_unlock(&lock);
  Reader:
    do {
        seq = READ_ONCE(seqcount);
        smp_rmb();
        data = READ_ONCE(protected_data);
        smp_rmb();
    } while (seq != READ_ONCE(seqcount) || seq & 1);

Pattern D: Ring buffer (kfifo style)
  Producer:
    buf[in & mask] = item;
    smp_wmb();               // data before index
    WRITE_ONCE(in, in+1);
  Consumer:
    n = READ_ONCE(in) - READ_ONCE(out);
    smp_rmb();               // index before data read
    item = buf[out & mask];
    smp_mb();                // data read before out update
    WRITE_ONCE(out, out+1);
```

---

## 3. Instruction Hierarchy (From Weakest to Strongest)

```
Ordering (CPU Memory):
  barrier()           weakest: compiler-only, no HW effect
  READ_ONCE/WRITE_ONCE: compiler barrier + no optimization
  LDAR/STLR           one-way acquire/release (directed barrier)
  DMB ISHLD/ISHST     load-only or store-only directional
  DMB ISH             full bidirectional (smp_mb)
  DSB ISH             full + pipeline stall + completion
  DSB SY              full system + all agents
  
Ordering (Device/MMIO):
  writel/readl        DSB built-in (ST or LD)
  wmb()/rmb()         DSB ST/LD system-scope
  mb()                DSB SY
  
Atomics (weakest to strongest):
  LDR/STR             plain load/store (no atomicity guarantee beyond word)
  LDXR/STXR           exclusive (LL/SC retry)
  CAS/SWP             single-instruction RMW (LSE, relaxed)
  CASAL/SWPAL         single-instruction RMW with acquire+release (LSE, SC)
```

---

## 4. Architecture Comparison

```
Feature             x86/x64    ARM64      POWER8
─────────────────────────────────────────────────
Store-Store ordered    YES       NO        NO
Load-Load ordered      YES       NO        NO
Store-Load ordered     NO        NO        NO
Data dependency        YES       YES       Partial
Atomic RMW (native)    Yes       LSE(v8.1) Yes
Hardware TSO           Yes       No        No

x86: Total Store Order — all stores ordered, loads ordered
     Only needs barriers for Store-Load (MFENCE = rare)
     smp_mb() on x86 = LOCK+ADD or MFENCE
     
ARM64: Weakly ordered — all barriers explicit
       smp_mb()  = DMB ISH (real hardware barrier)
       smp_rmb() = DMB ISHLD (real hardware barrier)
       smp_wmb() = DMB ISHST (real hardware barrier)

Impact: code that works on x86 without barriers may FAIL on ARM64!
  Common: drivers written on x86, ported to ARM64:
  "works on x86, fails on Raspberry Pi/server" → missing ARM64 barriers
```

---

## 5. Top 20 Interview Questions Summary

```
1. What is ARM64's memory ordering model? → Weakly Ordered (WO)

2. What are the four types of memory reordering? → LL, SS, LS, SL

3. When do you use DMB vs DSB? → DMB: ordering, DSB: completion + pipeline stall

4. What does ISB do? → Flushes instruction pipeline, forces context re-sync

5. When is ISB required? → After SCTLR, MAIR, TTBR changes; after I-cache invalidation

6. What's the difference between LDAR and DMB ISHLD? → LDAR: one-way acquire fence; DMB ISHLD: bidirectional load-load

7. How does smp_store_release work? → STLR: store with release, all earlier stores ordered before

8. What is data dependency ordering? → Load B using Load A's VALUE as ADDRESS → ARM64 orders them

9. Why does rcu_dereference() not need a barrier? → Data dependency: ptr deref creates address dependency → ordered on ARM64

10. What is CASAL? → CAS with Acquire+Release: sequentially consistent compare-and-swap (LSE)

11. What does WRITE_ONCE() do? → Volatile cast: prevents compiler caching, reordering, elimination

12. Explain LL/SC vs LSE CAS: → LL/SC = retry loop (can live-lock), LSE = hardware single instruction (bounded)

13. When to use wmb() vs smp_wmb()? → wmb() = DSB ST (for MMIO/device), smp_wmb() = DMB ISHST (CPU-only)

14. What happens if you miss DSB after DC CIVAC? → DMA may read from cache (not yet written to DRAM)

15. How does WFE reduce spinlock power? → CPU sleeps until STLR on lock address, no polling traffic

16. What is SEVL used for? → Priming WFE loop: prevents sleeping if condition already true

17. Difference between DMB ISH and DMB SY? → ISH: CPU scope; SY: full system including devices

18. What does dma_map_single(DMA_FROM_DEVICE) do? → DC IVAC: invalidates CPU cache for the buffer

19. Explain false sharing: → Two vars on same cache line, different CPUs write them, coherency traffic

20. How does seqlock prevent torn reads? → Uses sequence counter: odd=write in progress, reader retries if seq changed
```

---

## 6. Complete Barrier Decision Tree

```
Do you need a barrier?

Is this SAME CPU only? (single-threaded, IRQs disabled)
  YES → Use barrier() (compiler barrier only)
  NO  → Continue ↓

Is this for cache maintenance (DC CVAC, IC IVAU)?
  YES → Use DSB ISH (must stall pipeline for completion)
      → Follow with ISB if instruction fetch is affected

Is this for TLB invalidation (TLBI)?
  YES → DSB ISH before AND after TLBI; ISB after if code is affected

Is this for system registers (SCTLR, MAIR, TTBR)?
  YES → DSB ISH (if TLB involved) + ISB

Is this for MMIO / device registers?
  YES → Use writel/readl (DSB built-in)
       → Use wmb()/rmb() for explicit ordering (DSB ST/LD)

Is this CPU-to-DMA coherency?
  YES → Use dma_map_single / dma_unmap_single (DC CIVAC + DSB)

Is this CPU-to-CPU synchronization?
  What kind?
  - Store-Store only (producer writes data, then flag):
      smp_wmb() = DMB ISHST  OR  smp_store_release() = STLR
  - Load-Load only (consumer reads flag, then data):
      smp_rmb() = DMB ISHLD  OR  smp_load_acquire() = LDAR
  - Full (Store-Load, e.g., Dekker's):
      smp_mb() = DMB ISH (full bidirectional)
  - Lock/unlock critical section:
      Lock acquire: LDAXR (built into spin_lock)
      Lock release: STLR (built into spin_unlock)
      → Use spinlocks/mutexes correctly, no additional barriers needed inside CS
```

---

## 7. ARM64-Specific Gotchas

```
Gotcha 1: "This works on x86 → must work on ARM64"
  x86 TSO is stronger. Many patterns need explicit barriers on ARM64.
  ALWAYS test concurrent code on ARM64, not just x86.

Gotcha 2: Control dependency != Data dependency
  // Data dependency (SAFE on ARM64, no barrier):
  x = READ_ONCE(ptr);
  val = x->field;    // x's value used as ADDRESS → ordered
  
  // Control dependency (NOT safe, needs smp_rmb()):
  x = READ_ONCE(flag);
  if (x)
      val = READ_ONCE(data); // flag's VALUE used as CONDITION, not address
  // CPU may issue READ_ONCE(data) speculatively BEFORE the if!
  // Fix: add smp_rmb() inside the if before accessing data

Gotcha 3: DC CIVAC granularity
  DC CIVAC operates on CACHE LINE (64 bytes)
  Buffer not cache-aligned: overwrites neighboring non-DMA data's cache line!
  Always align DMA buffers to cache-line size.

Gotcha 4: IC IVAU without preceding DSB
  Sequence: DC CVAU; IC IVAU; DSB ISH; ISB  ← WRONG ORDER
  Correct:   DC CVAU; DSB ISH; IC IVAU; DSB ISH; ISB
  Without first DSB: DC CVAU may not have completed before IC IVAU executes!

Gotcha 5: Using WRITE_ONCE on struct (compile error)
  WRITE_ONCE is for scalar types only
  struct: use spinlock or per-field WRITE_ONCE + smp_wmb

Gotcha 6: Barriers in interrupt handlers
  Interrupt handlers run with IRQs disabled on that CPU
  But SMP: another CPU can still access shared data
  → Barriers still needed between interrupt handler and other CPUs!
```
