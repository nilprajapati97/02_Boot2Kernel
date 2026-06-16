# ARM64 Weak Memory Model: Foundation

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Memory ordering defines: in what order do MEMORY OPERATIONS become
VISIBLE to other observers (other CPUs, DMA devices, etc.)?

Sequential Consistency (SC) - intuitive but expensive:
  All memory operations appear to execute in PROGRAM ORDER
  All CPUs see ALL memory operations in the SAME global order
  Example: if CPU0 writes A=1 then B=1, CPU1 MUST see A=1 before B=1
  Cost: every load must drain the store buffer, every store is synchronous
  
ARM64 Memory Model: Weakly Ordered (WO):
  Loads and stores may be reordered by:
    1. The COMPILER (instruction scheduling)
    2. The HARDWARE (out-of-order execution, store buffer, load buffering)
  
  Result: the ORDER of memory accesses observed by OTHER CPUs may differ
  from the PROGRAM ORDER in which they were written
  
  Why weak ordering?
    - Higher performance: CPU can execute independent instructions out-of-order
    - Better memory bandwidth: store buffers coalesce writes before sending to cache
    - Speculative loads: CPU can issue loads early while waiting for address resolution
    
  The programmer's job: use BARRIERS to enforce ordering where required
  Barrier = a fence instruction that prevents certain classes of reordering

Key insight: the ARM Architecture Reference Manual defines ARM64 as
"multi-copy atomic" — a store becomes SIMULTANEOUSLY visible to all
observers ONCE it exits the store buffer to the cache hierarchy.
```

---

## 2. ARM64 Hardware Reordering Sources

```
Sources of memory reordering in ARM64:

1. Store Buffer:
   CPU writes: Store instruction → STORE BUFFER (fast, decoupled from cache)
   Store buffer → L1 D-cache: happens LATER (asynchronous)
   
   Effect: CPU can execute MULTIPLE STORES before first store reaches cache
   Other CPUs see stores in ARBITRARY order from their perspective
   
   Example:
     Store X=1 → enters store buffer slot 1
     Store Y=1 → enters store buffer slot 2
     ... both drain to cache (X=1, Y=1 visible), but WHICH drains first?
     On ARM64: no ordering guarantee without barriers
     
2. Load-Load reordering:
   CPU has load buffer for speculative loads
   Load A, then Load B: CPU may issue B's load BEFORE A's load completes
   If A and B are independent → reordering is safe for single CPU
   But: another CPU updating A THEN B may see old A + new B on our CPU!
   
3. Load-Store reordering:
   A store may become visible BEFORE an earlier load (from another CPU's view)
   
4. Write-combining buffer:
   Normal memory: WB (write-back) — stores coalesced in WC buffer
   Device memory: nGnRE — no combining (each access reaches device)
   
Reorderings NOT allowed on ARM64:
  ✓ Store-Load SAME address: ordered (coherent)
  ✓ Dependent loads: if B's address depends on A's VALUE, A ordered before B
  ✓ Data-dependent loads: respects data dependency
  
  ARM64 KEY RULE: if load B uses the VALUE loaded by load A to compute
  its ADDRESS, then A is guaranteed ordered before B. This is called
  ADDRESS DEPENDENCY or DATA DEPENDENCY ordering.
```

---

## 3. The Observer Model

```
ARM64 defines memory ordering from the perspective of OBSERVERS:

Observers:
  - CPU core executing loads/stores
  - Other CPU cores
  - DMA masters (PCIe devices, USB, DMA controllers)
  - I/O devices accessing MMIO

Observations:
  An observer "observes" a write when it can read the NEW value
  
Sequential Consistency example:
  Thread A (CPU0):  X = 1; Y = 1;
  Thread B (CPU1):  while (Y == 0); assert(X == 1);  // SAFE on SC
  
  On ARM64 WITHOUT barriers:
  Thread B sees Y=1 (wakes up) but X may STILL BE 0 from B's perspective
  (CPU0's store to X may be in its store buffer, not yet visible to CPU1)
  assert(X == 1) MAY FAIL on ARM64!
  
With DMB ISH barriers:
  Thread A (CPU0):  X = 1; DMB ISH; Y = 1;
  Thread B (CPU1):  while (Y == 0); DMB ISH; assert(X == 1);  // SAFE
  
  After CPU1 sees Y=1, the DMB ISH ensures CPU0's X=1 store
  has completed and is visible to all Inner Shareable observers
```

---

## 4. ARM64 Memory Model Categories

```
ARM64 Memory Types and their Ordering Guarantees:

Normal Cacheable memory (most RAM):
  - Permits: speculative loads, out-of-order execution, store buffer
  - Hardware enforces: same-address ordering, data dependency ordering
  - Programmer must use: DMB/DSB for cross-CPU ordering
  
Normal Non-Cacheable memory:
  - Like Normal Cacheable but: no L1/L2/L3 caching
  - Still allows: store buffer, load/store reordering
  - Common misconception: NC does NOT imply ordered!
  
Device memory (nGnRE, nGnRnE, GRE):
  - nG = non-Gathering: no write combining (each access is separate)
  - nR = non-Reordering: accesses to same peripheral region ordered
  - nE = non-Early: write acknowledgment only AFTER device receives it
  
  Device nGnRE: typical for MMIO registers
    - No gathering (separate accesses preserved)
    - No reordering (sequential within the device)
    - No early write completion (CPU waits for device ACK)
    
  Device nGnRnE: strictest (PCIe config space)
    - All three enforced
    
  Normal vs Device ordering:
    Device nGnRE is STRICTER than Normal memory for single-CPU access
    BUT: still needs DMB for cross-CPU (multi-observer) visibility!
```

---

## 5. Linux Kernel Memory Model (LKMM)

```
Linux has a formal memory model since 4.15:
  Documentation/memory-barriers.txt: informal description
  tools/memory-model/: formal LKMM (Linux Kernel Memory Model)
  
Linux primitives mapping to ARM64 instructions:
  
  smp_mb()    → DMB ISH     (full barrier, all loads/stores)
  smp_rmb()   → DMB ISHLD   (load-load barrier)
  smp_wmb()   → DMB ISHST   (store-store barrier)
  
  mb()        → DSB SY      (full system barrier, includes devices)
  rmb()       → DSB LD      (system load barrier)
  wmb()       → DSB ST      (system store barrier)
  
  READ_ONCE(x)   → use volatile-like access (prevent compiler reordering)
                   On ARM64: LDR (single atomic load)
  WRITE_ONCE(x,v)→ volatile-like assignment  
                   On ARM64: STR (single atomic store)
  
  smp_store_release(p, v) → STLR  (Store-Release)
  smp_load_acquire(p)     → LDAR  (Load-Acquire)

Why READ_ONCE/WRITE_ONCE?
  Without them: compiler may:
    1. Load the variable ONCE and cache it in a register (not re-read)
    2. Tear the access into multiple smaller operations
    3. Eliminate the load entirely (dead code elimination)
  READ_ONCE: ensures the compiler generates exactly ONE load instruction
  Prevents compiler from "seeing through" the access across barriers
```

---

## 6. Interview Questions & Answers

**Q1: Why is ARM64's memory model called "weakly ordered" and what are the practical consequences for kernel developers?**

ARM64's memory model is "weakly ordered" because the processor and hardware are allowed to reorder LOAD and STORE operations relative to their program order, as long as single-threaded correctness is maintained (register values are correct for the local CPU). The consequences for kernel developers are:

1. **Spinlocks and mutexes require barriers**: `spin_lock()`/`spin_unlock()` compile to acquire/release semantics (LDAR/STLR instructions) to prevent critical section operations from "leaking out" of the locked region.

2. **Message passing patterns need explicit barriers**: The common pattern "write data, then write flag" requires a store barrier (`smp_wmb()` or `smp_store_release()`) between the two writes, and the reader needs a load barrier after reading the flag.

3. **Interrupt handlers**: a device interrupt sets a flag; the main thread polling the flag needs `READ_ONCE()` to prevent the compiler from optimizing away the re-read.

4. **Device driver MMIO**: always use `writel()`/`readl()` (which include appropriate barriers) rather than direct pointer dereferences for MMIO registers.

---

## 7. Quick Reference

| Model | Allowed Reordering | Examples |
|---|---|---|
| Sequential Consistency | None | TSO + total order |
| Total Store Order (TSO) | Stores deferred in store buffer only | x86 |
| Partial Store Order | Load-load also reordered | SPARC PSO |
| Weakly Ordered | All non-dependent reordering | ARM64, POWER |

| ARM64 Memory Type | Gathering | Reordering | Early Write | Use Case |
|---|---|---|---|---|
| Normal WB | Yes | Yes | Yes | All RAM |
| Normal NC | Yes | Yes | Yes | DMA coherent |
| Device nGnRnE | No | No | No | PCIe config |
| Device nGnRE | No | No | Yes | MMIO registers |
| Device GRE | Yes | Yes | Yes | Frame buffer WC |
