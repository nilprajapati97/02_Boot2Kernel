# Load-Load, Store-Store, and Load-Store Orderings

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. The Four Reordering Types

```
Memory accesses between two CPUs can be reordered in 4 ways:

Type 1: LOAD-LOAD reordering (LL):
  CPU0 program order:  Load A → Load B
  CPU1 may observe:    Load B result visible before Load A result
  Example hazard:      A is a flag set by CPU1 after preparing data B
                       CPU0 reads B before A is old value! (stale B)

Type 2: STORE-STORE reordering (SS):
  CPU0 program order:  Store A=1 → Store B=1
  CPU1 may observe:    Store B=1 visible before Store A=1
  Example hazard:      A is data prepared, B is ready flag
                       CPU1 sees ready (B=1) but data (A) is still stale!

Type 3: LOAD-STORE reordering (LS):
  CPU0 program order:  Load A → Store B=1
  CPU1 may observe:    Store B=1 visible before Load A result affects CPU0
  Example hazard:      Load A used to decide whether to Store B=1
                       But B is stored even before A load is "committed"

Type 4: STORE-LOAD reordering (SL):
  CPU0 program order:  Store A=1 → Load B
  CPU1 may observe:    Load B reads OLD value even AFTER Store A=1
  Example hazard:      Dekker's mutual exclusion algorithm:
    CPU0: A=1; if(B==0) enter critical section  ← BROKEN on weak memory!
    CPU1: B=1; if(A==0) enter critical section
    Both CPUs may read 0 for the other's variable → BOTH enter critical section!

ARM64 guarantees:
  - Same-address coherency: always ordered (no CPU sees stale cache for own writes)
  - Data dependency: if Load A result used as address for Load B, order preserved
  ARM64 does NOT guarantee: LL, SS, LS, SL between different addresses!
```

---

## 2. Store-Store Ordering Deep Dive

```
Critical pattern: Producer-Consumer with flag

  Shared memory layout:
    data[0...N]  = actual payload
    ready_flag   = 1 when data is valid

Producer (CPU0):
  data[0] = compute_value();  // Store A
  data[1] = compute_value();  // Store B
  ready_flag = 1;             // Store C (flag)

Consumer (CPU1):
  while (!ready_flag);        // Load C (spin on flag)
  process(data[0], data[1]);  // Load A, Load B

WITHOUT barriers, CPU0's store buffer may drain:
  ready_flag=1 (Store C) → drains to L2 cache first!
  data[0], data[1] (Stores A,B) → still in CPU0's store buffer
  
  CPU1 sees ready_flag=1 → reads data[0], data[1] → STALE values!
  
WITH DMB ISHST (Store-Store barrier) on CPU0:
  data[0] = compute_value();    // Store A
  data[1] = compute_value();    // Store B
  smp_wmb();                    // DMB ISHST: A,B must be visible before C
  ready_flag = 1;               // Store C

WITH DMB ISHLD (Load-Load barrier) on CPU1:
  while (!ready_flag);          // Load C (flag)
  smp_rmb();                    // DMB ISHLD: C must be ordered before A,B loads
  process(data[0], data[1]);    // Load A, Load B

ARM64 assembly for producer:
  STR  w2, [x0]        // data[0] = value
  STR  w3, [x1]        // data[1] = value  
  DMB  ISHST           // store-store barrier, Inner Shareable
  MOV  w4, #1
  STR  w4, [x5]        // ready_flag = 1
```

---

## 3. Load-Load Ordering Deep Dive

```
Critical pattern: Flag-then-Data read

Consumer with load-load reordering hazard:

Without DMB ISHLD:
  LDR  w0, [x5]        // Load C: ready_flag
  CBZ  w0, spin        // if zero, spin
  LDR  w1, [x0]        // Load A: data[0]  ← may be REORDERED before Load C!
  LDR  w2, [x1]        // Load B: data[1]

Why this matters:
  CPU's out-of-order engine: may issue Load A and Load B speculatively
  BEFORE the ready_flag load completes (independent addresses)
  If CPU0's data[0] store is still in-flight → CPU1 gets stale data[0]
  even though ready_flag is now 1

With DMB ISHLD:
  LDR  w0, [x5]        // Load C: ready_flag
  CBZ  w0, spin        // spin if zero
  DMB  ISHLD           // load-load barrier: C must complete before A,B
  LDR  w1, [x0]        // Load A: data[0]  ← SAFE now
  LDR  w2, [x1]        // Load B: data[1]

Linux equivalent:
  while (!smp_load_acquire(&ready_flag));  // LDAR instruction on ARM64
  // After LDAR: loads below guaranteed to be ordered after the LDAR
  process(data[0], data[1]);
  
Alternatively:
  while (!READ_ONCE(ready_flag));
  smp_rmb();  // explicit DMB ISHLD
  val = READ_ONCE(data[0]);
```

---

## 4. Store-Load Ordering (Most Expensive)

```
Store-Load: the ONLY ordering that requires a FULL barrier even on x86 TSO!

Dekker's Algorithm (simplified):
  CPU0:
    X = 1;      // Store
    if (Y == 0) // Load
      critical_section_0();
      
  CPU1:
    Y = 1;      // Store  
    if (X == 0) // Load
      critical_section_1();

BROKEN on ARM64 without full barriers:
  CPU0: X=1 → store buffer (not yet visible to CPU1)
  CPU0: Load Y → Y is still 0 (CPU1's Y=1 is in CPU1's store buffer)
  CPU0: enters critical section!
  
  CPU1: Y=1 → store buffer (not yet visible to CPU0)
  CPU1: Load X → X is still 0 (CPU0's X=1 is in CPU0's store buffer)
  CPU1: enters critical section!
  
  BOTH in critical section! Mutual exclusion violated!

Fix: Full DMB ISH between Store and Load:
  CPU0:
    X = 1;
    DMB ISH;     // FULL barrier: drains store buffer, serializes
    if (Y == 0)
      critical_section_0();
      
  CPU1:
    Y = 1;
    DMB ISH;     // FULL barrier
    if (X == 0)
      critical_section_1();

Store-Load reordering is the MOST COMMON source of kernel bugs:
  Spinlock: spin_lock() and spin_unlock() use STLR/LDAR to prevent this
  RCU: rcu_read_lock() relies on acquire/release semantics
  Ring buffers (kfifo, io_uring): producer/consumer pairs need full barriers
```

---

## 5. ARM64 Instruction Types and Reordering

```
Instruction classification for memory ordering:

Normal load (LDR):
  May be issued speculatively before earlier loads/stores complete
  No ordering guarantee relative to other CPU's stores

Normal store (STR):
  Goes to store buffer → may be visible after later stores from same CPU
  No ordering guarantee

Load-Acquire (LDAR):
  All loads/stores BEFORE the LDAR are ordered before the LDAR
  All loads/stores AFTER the LDAR are ordered after the LDAR
  
  Actually: LDAR = no later loads/stores ordered before this load
  (One-way barrier: prevents later accesses from moving above)

Store-Release (STLR):
  All loads/stores BEFORE the STLR are ordered before the STLR
  All loads/stores AFTER the STLR are ordered after the STLR
  
  Actually: STLR = no earlier stores reordered after this store
  (One-way barrier: prevents earlier accesses from moving below)

RCpc (Release Consistency - Processor Consistent) - ARMv8.3:
  LDAPR (Load-Acquire RCpc): weaker acquire semantics
  Pairs with STLR (release)
  Allows store-load reordering at LDAPR (can be lifted above STLR)
  Use: one-way dependency only (cheaper on NUMA systems)

Combined LDAR/STLR effect:
  STLR (release) on CPU0 + LDAR (acquire) on CPU1:
  All stores before STLR on CPU0 visible to all stores/loads after LDAR on CPU1
  This is the acquire-release synchronization primitive (seqlocks, mutexes)
```

---

## 6. Linux Kernel Mapping Reference

```
Linux API → ARM64 instructions:

smp_mb()         → DMB ISH        (full Read+Write barrier)
smp_rmb()        → DMB ISHLD      (Read-Read barrier)
smp_wmb()        → DMB ISHST      (Write-Write barrier)

mb()             → DSB SY         (full system barrier, includes I/O)
rmb()            → DSB LD         (system read barrier)  
wmb()            → DSB ST         (system write barrier)

barrier()        → (compiler barrier only, no CPU instruction)
                   Prevents compiler reordering across this point

smp_load_acquire(p)      → LDAR (Load-Acquire)
smp_store_release(p, v)  → STLR (Store-Release)

atomic_read_acquire()    → LDAR  
atomic_set_release()     → STLR
atomic_cmpxchg_acquire() → CASAL or LDAXR/STLXR

// Typical flag-based message passing (kernel):
// Writer:
WRITE_ONCE(data, value);
smp_wmb();
WRITE_ONCE(ready, 1);

// Reader:
while (!READ_ONCE(ready));
smp_rmb();
val = READ_ONCE(data);

// Simplified with acquire/release:
// Writer:
WRITE_ONCE(data, value);
smp_store_release(&ready, 1);  // STLR → all previous stores visible

// Reader:
while (!smp_load_acquire(&ready));  // LDAR → all subsequent loads ordered
val = READ_ONCE(data);              // guaranteed to see updated data
```

---

## 7. Interview Questions & Answers

**Q1: What four types of memory reorderings exist on ARM64, and for each type, provide a concrete example of a bug it can cause?**

1. **Load-Load (LL)**: Consumer reads a flag and then reads data, but CPU speculatively issues the data load before the flag load completes. Bug: consumer sees the flag as 1 but reads stale data (data load completed before producer's stores).

2. **Store-Store (SS)**: Producer writes data then writes a ready flag, but the flag store drains from the store buffer before the data stores. Bug: consumer sees flag=1 but data fields still contain old values.

3. **Load-Store (LS)**: A load's result is supposed to gate a store, but the store is issued speculatively. Bug: data structures updated before a critical guard load determines they should be updated.

4. **Store-Load (SL)**: A store is issued but Load B's address is computed before the store becomes visible; Load B reads old value. Bug: Dekker's algorithm fails — both threads can enter the critical section simultaneously.

---

## 8. Quick Reference

| Reordering | Example Pattern | Barrier Needed | Linux API |
|---|---|---|---|
| Store-Store | Write data then flag | DMB ISHST | smp_wmb() |
| Load-Load | Read flag then data | DMB ISHLD | smp_rmb() |
| Load-Store | Read guard then store | DMB ISH | smp_mb() |
| Store-Load | Store then read other | DMB ISH | smp_mb() |
| All orderings | Critical section enter/exit | STLR / LDAR | spin_lock/unlock |
