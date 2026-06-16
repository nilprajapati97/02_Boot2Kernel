# LSE Atomics: CAS, CASP, SWP, and LL/SC vs LSE

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Atomic operations: read-modify-write operations that appear INDIVISIBLE
to all observers. No other CPU can see an intermediate state.

ARM64 provides two mechanisms:

1. Load-Link/Store-Conditional (LL/SC) — original ARM64:
   LDXR (Load Exclusive Register): loads and marks address as "exclusive"
   STXR (Store Conditional Register): stores ONLY if no other write happened
   If store fails: retry the entire read-modify-write
   
   Advantage: can compose any RMW operation (add, OR, AND, CAS, ...)
   Disadvantage: unbounded retry (live-lock possible), 
                 cannot be used in non-retryable contexts

2. Large System Extensions (LSE) — ARMv8.1:
   Single-instruction atomics: CAS, SWP, LDADD, LDSET, LDCLR, LDEOR, etc.
   These execute as TRUE HARDWARE atomics (no retry loop)
   
   Advantage: deterministic, cannot live-lock, composable without retry
   Disadvantage: limited to specific operations (no arbitrary RMW)
   
   ARM64 Linux preference: LSE when available (CONFIG_ARM64_LSE_ATOMICS)
   Runtime patching: __lse_ll_sc_body() alternates between implementations
```

---

## 2. LL/SC: LDXR/STXR Deep Dive

```
LDXR (Load Exclusive Register):
  LDXR Xt, [Xn]
  - Loads value from memory address [Xn] into Xt
  - Tags the address [Xn] as "exclusive monitor" for this CPU
  - Exclusive monitor: a 1-bit state machine per CPU
    (actually tracks a range of addresses, not exact address on most CPUs)
    
  Exclusive monitor states:
    Open access: no exclusive reservation
    Exclusive access: LDXR was performed (valid reservation)
    
  Events that CLEAR exclusive monitor (make subsequent STXR FAIL):
    - Another CPU writes to ANY address in the same exclusivity granule
    - Exception taken (IRQ, FIQ, SVC, ...) — clears the monitor
    - CLREX instruction (explicitly clear)
    - Another LDXR by same CPU (replaces old reservation)

STXR (Store Conditional Register):
  STXR Ws, Xt, [Xn]   (Ws = status register: 0=success, 1=failure)
  - Stores Xt to [Xn] ONLY IF exclusive monitor still valid
  - Returns status in Ws (always W-register, even for 64-bit store!)
  - On success: clears the exclusive monitor
  - On failure: Xt not stored, Ws=1 → caller must retry

Classic compare-and-swap using LL/SC:
  // cas(ptr, expected, new) → returns old value
  cas:
  1: LDXR   x3, [x0]      // load *ptr into x3 (exclusive)
     CMP    x3, x1        // compare with expected
     BNE    2f            // if different: done (CAS failed)
     STXR   w4, x2, [x0] // store new value if exclusive holds
     CBNZ   w4, 1b        // if store failed: retry
  2: MOV    x0, x3        // return old value
     RET
     
Exclusive monitor granule:
  ARM64: implementation-defined granule size (typically 64 bytes on Cortex-A)
  If ANY write within the same granule invalidates the exclusive:
    False contention possible! Two threads with adjacent data in same granule
    can interfere even when not sharing the same variable
  
  Mitigation: align atomic variables to cache-line (64 bytes):
    struct { int val; } __attribute__((aligned(64))) counter;
```

---

## 3. LSE: Large System Extensions (ARMv8.1)

```
LSE adds 16 new atomic instructions:

Compare-and-Swap:
  CAS   Ws, Wt, [Xn]     // 32-bit: if *Xn == Ws then *Xn = Wt; Ws = old value
  CAS   Xs, Xt, [Xn]     // 64-bit
  CASH  Ws, Wt, [Xn]     // 16-bit halfword
  CASB  Ws, Wt, [Xn]     // 8-bit byte
  CASP  Ws, Wt, [Xn]     // 128-bit pair (pair of 64-bit registers)
  
  Acquire/Release variants: CASA, CASL, CASAL
  CASA   = CAS with Acquire (load has acquire semantics)
  CASL   = CAS with Release (store has release semantics)
  CASAL  = CAS with both Acquire AND Release (fully sequentially consistent)

Atomic fetch-and-add:
  LDADD  Ws, Wt, [Xn]    // Wt = *Xn; *Xn += Ws (32-bit)
  LDADD  Xs, Xt, [Xn]    // Xt = *Xn; *Xn += Xs (64-bit)
  LDADDH, LDADDB: halfword, byte variants
  LDADDA, LDADDL, LDADDAL: acquire/release variants
  
  For fetch-and-add where you don't need return value:
  STADD  Ws, [Xn]  // *Xn += Ws (no return, may be slightly cheaper)

Atomic bit manipulation:
  LDSET  Ws, Wt, [Xn]    // Wt = *Xn; *Xn |= Ws (fetch-and-OR)
  LDCLR  Ws, Wt, [Xn]    // Wt = *Xn; *Xn &= ~Ws (fetch-and-AND-NOT)  
  LDEOR  Ws, Wt, [Xn]    // Wt = *Xn; *Xn ^= Ws (fetch-and-XOR)
  STSET, STCLR, STEOR: no-return variants

Atomic swap:
  SWP   Ws, Wt, [Xn]    // Wt = *Xn; *Xn = Ws (exchange)
  SWPA, SWPL, SWPAL: acquire/release variants
```

---

## 4. LL/SC vs LSE: Detailed Comparison

```
Performance comparison (Cortex-A78):

                    LL/SC           LSE
Uncontended:        ~15 cycles      ~15 cycles (similar for single thread)
Moderate contention:~50 cycles      ~25 cycles (LSE: hardware arbiter)
High contention:    ~200+ cycles    ~40 cycles (LSE: no live-lock!)
16-core, 1 variable: live-lock!     bounded latency

Why LSE is faster under contention:
  LL/SC: software retry loop → CPU0 and CPU1 may BOTH retry at same time
         → they both fail → both retry again → "live-lock" pattern
         
  LSE CAS: hardware arbitrates between competing requests at L3 cache
           Only ONE wins, loser gets old value immediately → retry once, success
           No exponential backoff needed

Live-lock example (LL/SC, theoretical):
  CPU0: LDXR x3,[x]  CPU1: LDXR x3,[x]  // both get exclusive
  CPU0: STXR w4,[x]  → fails (CPU1 invalidated CPU0's monitor)
  CPU1: STXR w4,[x]  → fails (CPU0 invalidated CPU1's monitor)
  CPU0: retry...     CPU1: retry...      → infinite loop!
  
  In practice: unlikely but theoretically possible
  LSE: CAS is true atomic → never live-locks

Linux atomic implementation (arch/arm64/include/asm/atomic_lse.h):

// atomic_inc() using LSE LDADD:
static inline void atomic_inc(atomic_t *v) {
    __lse_ll_sc_body(atomic_inc, v);
}

__asm__ volatile("staddl %w[i], %[v]\n"
                 : [v] "+Q" (v->counter)
                 : [i] "r" (1));

// STADDL: store-add with Release semantics (no need to return value)
// The 'l' suffix: LDADDL/STADDL includes Release → matches atomic_inc expected ordering

// atomic_cmpxchg() using LSE CAS:
static inline int atomic_cmpxchg(atomic_t *v, int old, int new) {
    __asm__ volatile("casal %w[old], %w[new], %[v]"
                     : [old] "+r" (old), [v] "+Q" (v->counter)
                     : [new] "r" (new));
    return old;  // old is modified by CASAL to contain the ACTUAL old value
}
// CASAL: compare-and-swap with Acquire+Release (sequentially consistent)
```

---

## 5. CASP: 128-bit Pair Atomic

```
CASP (Compare and Swap Pair) — unique to ARM64:
  CASP  Xs, Xs+1, Xt, Xt+1, [Xn]
  
  Reads two 64-bit values from [Xn] and [Xn+8] (128 bits total)
  Compares with Xs:Xs+1 (pair of registers)
  If equal: atomically stores Xt:Xt+1 to [Xn]:[Xn+8]
  Xs:Xs+1 = old values read
  
  Requirement: Xn must be 16-byte aligned
  
  Use case: lock-free data structures using double-word CAS
  
  Example: lock-free stack with ABA problem prevention
  
  struct node { struct node *next; u64 version; };
  struct stack { struct node *head; u64 version; };  // 16-byte!
  
  // Compare-and-swap on BOTH head pointer AND version atomically
  CASP X0, X1, X2, X3, [Xsp]
  // X0=expected_head, X1=expected_version
  // X2=new_head, X3=new_version
  // [Xsp] = stack's head+version pair (16-byte aligned!)
  
  This atomically:
    IF (stack.head == expected_head AND stack.version == expected_version):
        stack.head = new_head; stack.version = new_version;
  
  The VERSION counter prevents ABA: even if head pointer returns to same value
  (A → B → A), the version is different → CASP detects the change

Linux uses of CASP:
  arch/arm64/include/asm/atomic_lse.h: cmpxchg_double() → CASP
  Used in: lock-free per-CPU structures, mmu_gather batching
```

---

## 6. Ordering Variants Reference

```
LSE instruction naming convention:
  Base: CAS, SWP, LDADD, LDSET, LDCLR, LDEOR
  Acquire suffix A: load has Acquire semantics
  Release suffix L: store has Release semantics
  Both AL: fully sequentially consistent
  
  Examples:
    CAS    = no ordering (relaxed)
    CASA   = compare-and-swap with Acquire
    CASL   = compare-and-swap with Release
    CASAL  = compare-and-swap with both (SC)
    
    LDADD  = fetch-and-add, relaxed
    LDADDA = fetch-and-add, Acquire
    LDADDL = fetch-and-add, Release
    LDADDAL= fetch-and-add, Acquire+Release

Linux mapping:
  atomic_read()              → LDR (relaxed load)
  atomic_set()               → STR (relaxed store)
  atomic_add_return()        → LDADDAL (acquire+release)
  atomic_inc()               → STADDL  (release only — "add 1, no return")
  atomic_cmpxchg()           → CASAL  (acquire+release)
  atomic_fetch_and()         → LDCLRAL (fetch-and-clear = AND-NOT)
  atomic_fetch_or()          → LDSETALAL
  atomic64_xchg()            → SWPAL  (swap, acquire+release)
```

---

## 7. Interview Questions & Answers

**Q1: Why can LL/SC (LDXR/STXR) potentially live-lock on ARM64, and how does LSE CAS solve this problem?**

LL/SC live-lock occurs when two CPUs simultaneously execute the retry loop. Both CPUs load exclusive (LDXR), both execute their read-modify-write operation locally, then both attempt STXR. CPU0's STXR fails because CPU1's LDXR invalidated CPU0's exclusive monitor. CPU1's STXR also fails because CPU0's LDXR invalidated CPU1's exclusive monitor. Both CPUs retry simultaneously: both succeed with LDXR again, both fail with STXR. This pattern can theoretically repeat indefinitely.

In practice, ARM64 CPUs include contention avoidance: the LDXR/STXR "exclusivity granule" is typically cache-line sized, and CPUs implement fairness at the cache coherency level. But with many cores and a hot atomic variable (e.g., a global counter), the retry count can become very large and throughput collapses.

LSE CAS (`CASAL`) is a single instruction that is handled **atomically in hardware** — typically at the L3 cache slice holding the cache line. The cache controller arbitrates between competing CAS operations and processes them one at a time. Each CAS either succeeds (old value matches expected) or fails immediately with the actual old value — the CPU never needs to retry due to another CPU's interference on the exclusivity monitor. The hardware guarantees bounded latency even with all 16 cores hitting the same address.

---

## 8. Quick Reference

| Instruction | Operation | Ordering |
|---|---|---|
| LDXR/STXR | LL/SC (retry loop) | Explicit with barriers |
| LDAXR/STLXR | LL/SC with acquire/release | Built-in acquire/release |
| CAS | Compare-and-swap | Relaxed |
| CASAL | CAS acquire+release | Sequentially consistent |
| LDADD | Fetch-and-add | Relaxed |
| LDADDAL | Fetch-and-add | Acquire+release |
| SWP | Atomic swap | Relaxed |
| SWPAL | Atomic swap | Acquire+release |
| CASP | 128-bit pair CAS | — |

| Scenario | Prefer | Reason |
|---|---|---|
| Simple inc/dec counter | LDADDAL/STADDL | Faster under contention |
| Boolean lock flag | SWP/SWPAL | Clean exchange semantics |
| CAS loop | CASAL | No live-lock |
| Complex RMW (bit fiddle) | LDXR/STXR | More flexible |
| Double-word atomic | CASP | No other option |
