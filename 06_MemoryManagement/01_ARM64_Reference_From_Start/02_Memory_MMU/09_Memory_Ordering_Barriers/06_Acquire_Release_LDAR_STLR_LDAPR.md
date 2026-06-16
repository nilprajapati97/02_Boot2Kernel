# Acquire/Release Semantics: LDAR, STLR, LDAPR

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Acquire/Release is a synchronization primitive from the C11/C++11 memory model.
ARM64 provides dedicated hardware instructions for acquire/release semantics.

RELEASE semantics (on a WRITE / STORE):
  All memory operations BEFORE the release store are ORDERED BEFORE the store
  The store "releases" the critical section — all writes before it are visible
  
  Analogy: "I'm done with the critical section. Everything I did is now visible."
  
  ARM64 instruction: STLR (Store-Release Register)

ACQUIRE semantics (on a READ / LOAD):
  All memory operations AFTER the acquire load are ORDERED AFTER the load
  The load "acquires" ownership — all reads after it use fresh state
  
  Analogy: "I'm entering the critical section. Everything I see is up-to-date."
  
  ARM64 instruction: LDAR (Load-Acquire Register)

Together: STLR + LDAR form a synchronization pair
  CPU0 does STLR (release) → CPU1 does LDAR (acquire) →
  All of CPU0's operations before STLR are visible to CPU1 after LDAR

Visual model:
  CPU0:  [A=1][B=2][C=3]  STLR(flag=1)  [D=4][E=5]
                         ↑ NOTHING above leaks below
  CPU1:                          LDAR(flag) → if 1:
                                             ↓ NOTHING below moves above
                                   [read A][read B][read C]  ← all see new values!
```

---

## 2. STLR (Store-Release Register)

```
STLR <Wt>, [<Xn>]  (32-bit variant)
STLR <Xt>, [<Xn>]  (64-bit variant)

Semantics:
  All explicit memory accesses BEFORE the STLR are guaranteed to be
  OBSERVED before the STLR's store is observed by any other observer
  
  One-way barrier: DOWNWARD — earlier accesses cannot move past STLR
  (Accesses AFTER STLR may still be reordered relative to each other)
  
  STLR guarantees:
    Load A; Load B; Store X = 1; STLR Y = flag;
    Any CPU that observes Y=flag MUST also observe: Load A/B completed,
    Store X=1 visible (happens-before the STLR)
    
Comparison with STR + DMB:
  STR + DMB ISH ISHST:
    Functionally equivalent to STLR (mostly)
    STR followed by DMB ISHST is actually slightly weaker on some ARM definitions
    
  ARM64 formal model: STLR is "Single-copy atomic" Release store
  STLR is the PREFERRED way to express release semantics
  
  Some ARM64 microarchitectures: STLR = STR + DMB ISHST (same latency)
  Others: STLR has dedicated hardware path (slightly cheaper than STR+DMB)

Usage in Linux:
  smp_store_release(&x, val) → STLR Wt, [Xn]
  
  spinlock unlock:
    arch_spin_unlock():
      STLR WZR, [lock]   ← store 0 (unlock) with Release semantics
      (Actually stores the new value; ZR means clear the lock)
      
  rcu_assign_pointer(p, v):
    smp_store_release(&p, v)  → STLR
```

---

## 3. LDAR (Load-Acquire Register)

```
LDAR <Wt>, [<Xn>]  (32-bit variant)
LDAR <Xt>, [<Xn>]  (64-bit variant)

Semantics:
  All explicit memory accesses AFTER the LDAR are guaranteed NOT to be
  OBSERVED before the LDAR's load is observed
  
  One-way barrier: UPWARD — later accesses cannot move before LDAR
  (Accesses BEFORE LDAR may still be reordered relative to each other)
  
  LDAR guarantees:
    LDAR x0 (flag); Load A; Load B;
    Load A and Load B cannot be reordered to appear before the LDAR
    If LDAR returned the new flag value, Loads A and B are guaranteed to
    see all stores that were done before the matching STLR
    
Comparison with DMB + LDR:
  DMB ISHLD followed by LDR: similar semantics
  LDAR is preferred: single instruction, clearer semantics, often cheaper
  
  IMPORTANT: LDAR also prevents LOAD FORWARDING from before the LDAR
  This is called "load-acquire exclusivity" — newer ARM specs define this carefully

Usage in Linux:
  smp_load_acquire(&x) → LDAR Wt, [Xn]
  
  spinlock lock:
    arch_spin_lock(): uses LDAXR (Load-Acquire Exclusive) in LL/SC loop
    The Acquire in LDAXR ensures critical section starts with fresh values
    
  rcu_dereference(p):
    lockless_dereference(p):
      READ_ONCE(p) + smp_read_barrier_depends()
      On ARM64: READ_ONCE sufficient (data dependency ordering)
      
  wait_event() macros: use smp_load_acquire for the condition

ARM64 register pairs:
  LDAR/STLR:  64-bit and 32-bit scalar loads/stores
  LDARH/STLRH: 16-bit halfword
  LDARB/STLRB: 8-bit byte
```

---

## 4. LDAPR (Load-Acquire RCpc)

```
LDAPR: Load-Acquire Register (Release Consistent Processor Consistent)
ARMv8.3+ FEAT_LRCPC

LDAPR: WEAKER acquire semantics than LDAR
  LDAR:  one-way "full" acquire (accesses after cannot move before LDAR)
  LDAPR: one-way "RCpc" acquire (accesses after can move before paired STLR!)
  
  Technical difference:
  LDAR pairs with STLR → strict ordering
  LDAPR pairs with STLR → allows some reordering between LDAPR and STLR from ANOTHER CPU
  
  In practice for most Linux usage: LDAR and LDAPR are equivalent
  LDAPR can be used when the acquire is a "one-way" (not pairing with a release from another thread in the same direction)

When LDAPR is useful:
  Critical sections where you only need: stores from BEFORE the release
  are visible AFTER the acquire — but you don't need the full fence of LDAR
  
  ARM64 performance: on some implementations LDAPR is CHEAPER than LDAR
  because LDAPR allows more out-of-order execution
  
  ARMv8.3 LDAPR: 64, 32, 16, 8-bit variants
  LDAPR <Xt>, [<Xn|SP>]
  LDAPRH <Wt>, [<Xn|SP>]
  LDAPRB <Wt>, [<Xn|SP>]

Linux LDAPR:
  smp_cond_load_acquire() and atomic_load_acquire() may use LDAPR on CPUs with FEAT_LRCPC
  CONFIG_ARM64_USE_LSE_ATOMICS: uses LDAPR when available

ARMv8.4 FEAT_LRCPC2: adds LDAPUR (LDAPR with unscaled offset)
  Allows LDAPR with immediate offset (more efficient for struct field access)
```

---

## 5. Acquire/Release vs Full Barriers

```
Performance comparison:

Full barrier (smp_mb = DMB ISH):
  Bidirectional: orders ALL before AND ALL after
  Cost: drains store buffer BOTH ways
  Example: 50 cycles (Cortex-A78)
  
One-way barriers (LDAR/STLR):
  Unidirectional: acquire stops later from moving up, release stops earlier from moving down
  Cost: cheaper on some microarchitectures (no bidirectional drain)
  Example: 15–30 cycles (Cortex-A78)
  
Comparison:
  // Using full barriers (more expensive):
  smp_wmb();               // DMB ISHST
  WRITE_ONCE(*flag, 1);    // STR
  
  // vs using STLR (preferred):
  smp_store_release(flag, 1);  // STLR (single instruction, one-way barrier)
  
  // On consumer:
  while (!READ_ONCE(*flag));   // wait
  smp_rmb();                   // DMB ISHLD (extra barrier needed!)
  val = READ_ONCE(*data);      // LDR
  
  // vs LDAR (preferred):
  while (!smp_load_acquire(flag));  // LDAR (barrier built in, one instruction!)
  val = READ_ONCE(*data);           // guaranteed ordered after LDAR

When full barrier needed (STLR/LDAR NOT sufficient):
  Store-Load ordering (Dekker's algorithm):
    X = 1;
    DMB ISH;   // FULL BARRIER (DMB ISH, not STLR)
    if (Y == 0) critical_section();
    
  STLR does NOT provide: ordering between the STLR store and a LOAD after it
  (Store-Load reordering: STLR allows loads after STLR to be reordered before STLR)
  
  For Dekker: use smp_mb() = DMB ISH (full barrier)
  For producer-consumer: STLR + LDAR is sufficient and cheaper
```

---

## 6. Spinlock Implementation Using LDAR/STLR

```
ARM64 Linux spinlock (arch/arm64/include/asm/spinlock.h):

struct arch_spinlock {
    union {
        u32 slock;
        struct { u16 owner, next; } tickets;
    };
};

arch_spin_lock() — ticket spinlock with LDAXR/STXR:
  PRFM PSTL1STRM, [x0]     // prefetch for streaming store
  
1: LDAXR  W2, [X0]          // Load-Acquire Exclusive: load lock value
   LDRH   W3, W2 >> 16       // extract 'next' field  
   LDRH   W4, W2 & 0xFFFF    // extract 'owner' field
   ADD    W5, W2, #(1 << 16) // increment next ticket
   STXR   W6, W5, [X0]      // Store Exclusive (no Release, need exclusivity)
   CBNZ   W6, 1b             // retry if store failed
   
   // Wait for our ticket to be served:
   CMP    W3, W4            // if next == owner: we have the lock
   BEQ    acquired
2: WFE                       // wait for event (reduce cache snoop traffic)
   LDAXRH W4, [X0]           // Load-Acquire Exclusive Half: re-read owner
   CMP    W3, W4
   BNE    2b
acquired:

arch_spin_unlock():
  STLRH  WZR, [X0]           // Store-Release Half: increment owner
                              // STLR ensures critical section visible before unlock

Key: LDAXR (Load-Acquire-Exclusive) + STXR (Store-Exclusive)
     Lock ACQUIRE: LDAXR provides the acquire barrier
     Lock RELEASE: STLR provides the release barrier
     No separate DMB needed in the hot path!
```

---

## 7. Interview Questions & Answers

**Q1: Explain the difference between STLR and STR+DMB ISHST. Are they semantically equivalent?**

**Not exactly equivalent.** `STR` followed by `DMB ISHST` gives store-store ordering: all stores before the `DMB ISHST` are ordered before stores after it. `STLR` provides release semantics that additionally ensures that all LOADS (not just stores) before the `STLR` are visible before the `STLR`'s store. So `STLR` ≈ `STR` followed by `DMB ISH` (full, not ISHST).

However, in the context of paired `STLR` + `LDAR` (release + acquire), the guarantee is stronger than just `STR` + `DMB ISH` + `LDR`. The `STLR`/`LDAR` pair provides the full "release-acquire" synchronization guarantee defined in the ARM64 memory model and C11/C++11 memory model. This is why Linux prefers `smp_store_release()`/`smp_load_acquire()` over separate `WRITE_ONCE()` + `smp_wmb()` + `READ_ONCE()` — it's both more correct and potentially more efficient.

---

## 8. Quick Reference

| Instruction | Direction | What It Orders |
|---|---|---|
| LDAR (Load-Acquire) | ↓ (one-way) | Nothing after moves before LDAR |
| STLR (Store-Release) | ↑ (one-way) | Nothing before moves after STLR |
| DMB ISH (full) | ↕ (two-way) | Full bidirectional barrier |
| LDAPR (RCpc acquire) | ↓ (weak) | Weaker than LDAR, cheaper |

| Pattern | Linux API | ARM64 Instruction |
|---|---|---|
| Release a value | smp_store_release() | STLR |
| Acquire a value | smp_load_acquire() | LDAR |
| Spin lock unlock | spin_unlock() | STLRH (ticket) |
| Spin lock lock | spin_lock() | LDAXR+STXR (LL/SC) |
| Full fence | smp_mb() | DMB ISH |
