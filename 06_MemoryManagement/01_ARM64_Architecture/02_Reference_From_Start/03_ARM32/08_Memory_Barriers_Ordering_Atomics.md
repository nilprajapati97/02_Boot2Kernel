# ARM32 Memory Barriers, Ordering, and Atomics
## Document 8: Weak Memory Model, DMB/DSB/ISB, LDREX/STREX, Linux Kernel Synchronization

**Author:** Senior Kernel Engineer  
**Target:** ARMv7-A (ARM32), Linux Kernel v5.x/v6.x  
**Scope:** ARM memory model, barrier instructions, exclusive monitors, Linux sync primitives  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview)

---

## Table of Contents
1. [ARM Memory Model — Weakly Ordered](#1-arm-memory-model--weakly-ordered)
2. [Memory Ordering Instructions — DMB, DSB, ISB](#2-memory-ordering-instructions--dmb-dsb-isb)
3. [Memory Barrier Options and Scopes](#3-memory-barrier-options-and-scopes)
4. [Exclusive Monitors — LDREX/STREX](#4-exclusive-monitors--ldrexstrex)
5. [Atomic Operations in ARM32](#5-atomic-operations-in-arm32)
6. [Linux Kernel Memory Barrier API](#6-linux-kernel-memory-barrier-api)
7. [Linux Atomic Operations API](#7-linux-atomic-operations-api)
8. [Spinlocks and Mutexes — ARM32 Implementation](#8-spinlocks-and-mutexes--arm32-implementation)
9. [Memory Types and Barrier Requirements](#9-memory-types-and-barrier-requirements)
10. [Common Barrier Bugs](#10-common-barrier-bugs)
11. [Lock-Free Data Structures on ARM32](#11-lock-free-data-structures-on-arm32)
12. [Performance Considerations](#12-performance-considerations)

---

## 1. ARM Memory Model — Weakly Ordered

### 1.1 What "Weakly Ordered" Means

```
x86 (Strongly Ordered — TSO — Total Store Order):
  CPU executes stores and loads in program order (to other processors).
  Hardware provides strong guarantees automatically.
  Barriers rarely needed (only for specific cases like MFENCE).

ARM (Weakly Ordered):
  The CPU can reorder memory accesses freely for performance.
  Other processors/DMA may see accesses in different order than program.
  Software MUST use explicit barriers where ordering matters.

Example — Producer/Consumer:
  Producer (CPU0):          Consumer (CPU1):
  data[0] = 42;             while (!ready) {}
  data[1] = 99;             val0 = data[0];   ← can be 0!
  ready = 1;                val1 = data[1];   ← can be 0!

ARM32 without barriers:
  CPU0 may write ready=1 BEFORE data[] (store reordering)
  CPU1 may read data[] BEFORE ready (load reordering)
  Result: CPU1 sees ready=1 but data[0]=0, data[1]=0
```

### 1.2 ARM Memory Ordering Rules

```
ARM guarantees the following ordering rules:

1. SAME ADDRESS ordering:
   Load/store to the same address: always in program order.
   
2. DATA DEPENDENCY ordering:
   If address of second access depends on result of first:
     Load r0, [r1]       ← r0 = value from r1
     Load r2, [r0]       ← address depends on r0 → ordered
   ARM respects data dependencies (unlike Alpha architecture).
   
3. NORMAL MEMORY can be reordered:
   Loads with different addresses can be reordered among themselves.
   Stores with different addresses can be reordered among themselves.
   Loads and stores to different addresses can be reordered.
   
4. STRONGLY-ORDERED and DEVICE memory:
   Never reordered relative to other Strongly-Ordered or Device accesses.
   Provides some built-in ordering guarantees.
   
5. WRITE BUFFER:
   CPU may have a write buffer (store buffer) that delays writes.
   A subsequent load from a different address can overtake a buffered write.
```

### 1.3 Reordering Taxonomy

```
Four types of reordering hazards:

1. StoreStore (SS): CPU reorders two stores
   CPU:  store A=1, store B=1
   Observer sees: B=1, A=0 (wrong order)
   Fix: DMB between stores

2. LoadLoad (LL): CPU reorders two loads
   CPU:  load r0=B, load r1=A
   CPU might read A before B completes
   Fix: DMB between loads

3. LoadStore (LS): CPU issues store before earlier load completes
   CPU:  load r0=A, store B=1
   B may be visible before load from A completes
   Fix: DMB

4. StoreLoad (SL): Most expensive — CPU reads before write visible
   CPU:  store A=1, load r0=B
   CPU may speculatively load B before A is written
   Fix: DMB (or full barrier)
   
Note: x86 only allows SL reordering (and only with stores, not between programs).
      ARM allows all four types.
```

---

## 2. Memory Ordering Instructions — DMB, DSB, ISB

### 2.1 DMB — Data Memory Barrier

```assembly
DMB   (or DMB ISH in SMP code)

Purpose: Ensures all memory accesses BEFORE the DMB are observed
         (by other cores/DMA) before any memory accesses AFTER DMB.
         
Does NOT:
  - Ensure in-order instruction execution
  - Ensure cache maintenance completes
  - Flush branch predictor
  
When to use:
  - Between stores in a producer:
      store data, DMB, store flag
  - Between flag check and data read in consumer:
      load flag, DMB, load data
  - Between DMA mapping and trigger:
      flush_cache(), DMB, writel(DMA_START)

Cost: Typically 10-50 cycles (waits for write buffer to drain to PoC)
```

### 2.2 DSB — Data Synchronization Barrier

```assembly
DSB   (or DSB ISH in SMP code)

Purpose: STRONGER than DMB.
  - Waits for all memory accesses to complete
  - Waits for all CP15 register writes to take effect
  - Waits for TLB invalidations to complete
  - Waits for cache maintenance operations to complete
  - Ensures instruction cache coherency is visible to CPU
  - No instruction after DSB executes until DSB completes
  
When to use:
  - After TLB invalidation (MCR TLBI* → DSB)
  - After cache maintenance operations
  - After writing to CP15 registers (TTBR0, SCTLR, etc.)
  - Before MMU enable/disable
  - At low-power state transitions
  
Cost: 50-200 cycles (much heavier than DMB — waits for system idle)
```

### 2.3 ISB — Instruction Synchronization Barrier

```assembly
ISB

Purpose: Flushes the CPU pipeline.
  - All instructions fetched BEFORE ISB are discarded
  - Instructions AFTER ISB are re-fetched with current MMU/CP15 state
  
Does NOT affect memory ordering (that's DMB/DSB).

When to use:
  - After writing SCTLR.M (MMU enable/disable)
  - After writing TTBR0/TTBR1 (page table base change)
  - After writing VBAR (vector base address)
  - After writing SCTLR.I (instruction cache enable)
  - After writing CPSR (mode change)
  - After writing to instruction memory (JIT, patching)
  
Cost: Variable (~5-10 cycles — flushes pipeline, refetch from new state)
```

### 2.4 Comparison Table

| Property | DMB | DSB | ISB |
|----------|-----|-----|-----|
| Memory access ordering | ✓ | ✓ (stronger) | — |
| Waits for CP15 writes | — | ✓ | — |
| Waits for cache ops | — | ✓ | — |
| Waits for TLB ops | — | ✓ | — |
| Flushes pipeline | — | — | ✓ |
| Typical cost (cycles) | 10-50 | 50-200 | 5-10 |

---

## 3. Memory Barrier Options and Scopes

### 3.1 DMB/DSB Shareability Options

```assembly
/* Scope: which observers must see the barrier's effect */

DMB SY   / DSB SY    @ Full system (all observers: CPUs, DMA, GPU)
DMB ISH  / DSB ISH   @ Inner Shareable (all CPUs in same cluster)
DMB NSH  / DSB NSH   @ Non-Shareable (only this CPU — effectively no barrier)
DMB OSH  / DSB OSH   @ Outer Shareable (all CPUs + outer shareable devices)

/* Type: which accesses the barrier applies to */
DMB LD   / DSB LD    @ Loads only (LoadLoad + LoadStore ordering)
DMB ST   / DSB ST    @ Stores only (StoreStore ordering)
DMB ISH  / DSB ISH   @ All (Loads + Stores)

/* Combinations: */
DMB ISHLD            @ Inner shareable, loads only
DMB ISHST            @ Inner shareable, stores only

/* SMP code: use ISH (most common in Linux kernel) */
/* DMA code: use SY or OSH (ensure DMA engine sees the barrier) */
```

### 3.2 Assembly Examples

```assembly
/* SMP: Publish data before flag */
producer:
    STR  r1, [r2]      @ store data
    DMB  ISH            @ ensure data visible before flag
    MOV  r3, #1
    STR  r3, [r4]      @ store flag = 1

/* SMP: Consume flag before data */
consumer:
loop:
    LDR  r3, [r4]      @ load flag
    CMP  r3, #1
    BNE  loop          @ spin
    DMB  ISH            @ ensure flag load complete before data load
    LDR  r1, [r2]      @ load data — guaranteed to be 42

/* Device: Write to device register after data buffer */
device_trigger:
    flush_dcache_range(buf, buf+size)   @ flush CPU cache
    DSB  SY             @ wait for cache flush to reach DRAM
    WRITEL(DMA_ADDR, buf_pa)
    WRITEL(DMA_LEN, size)
    DSB  SY             @ ensure address/len visible to DMA engine
    WRITEL(DMA_CTRL, DMA_START)     @ trigger DMA
```

### 3.3 Barrier After CP15 Writes

```assembly
/* Sequence: update TTBR0 and ensure new translations used */

/* 1. Write CONTEXTIDR (ASID) */
MCR  p15, 0, r1, c13, c0, 1   @ CONTEXTIDR = new ASID
ISB                              @ pipeline flush — ASID now active

/* 2. Write TTBR0 (new page table) */
MCR  p15, 0, r0, c2, c0, 0   @ TTBR0 = new PGD
ISB                              @ pipeline flush — new PGD now active

/* 3. Enable MMU (SCTLR.M = 1) */
MRC  p15, 0, r0, c1, c0, 0   @ Read SCTLR
ORR  r0, r0, #1               @ Set M bit
DSB                              @ ensure all previous accesses complete
MCR  p15, 0, r0, c1, c0, 0   @ Write SCTLR (MMU enable)
ISB                              @ flush pipeline — now running with MMU
```

---

## 4. Exclusive Monitors — LDREX/STREX

### 4.1 Exclusive Access Hardware

```
ARM32 exclusive access mechanism:
  Replaces traditional "test-and-set" (which requires bus locking)
  Allows lock-free atomic operations on any memory

Hardware Monitors:
  Local Monitor:  Per-CPU, tracks exclusive accesses to any address
  Global Monitor: Shared bus monitor, tracks accesses to Shareable memory
  
  For Non-Shareable memory: only Local Monitor used
  For Inner Shareable memory: Local + Global Monitor

Exclusive monitor state: "Open" or "Exclusive"
  LDREX: sets monitor to Exclusive for the accessed address
  STREX: attempts store; if monitor still Exclusive → succeeds (returns 0)
                          if monitor was cleared → fails (returns 1)
  
  Monitor is cleared by:
    - Successful STREX by any CPU
    - CLREX instruction
    - Exception entry/return
    - Context switch (kernel calls CLREX)
```

### 4.2 LDREX/STREX Instructions

```assembly
/* LDREX — Load Exclusive */
LDREX  Rt, [Rn]         @ Load word, set exclusive monitor
LDREXB Rt, [Rn]         @ Load byte exclusive
LDREXH Rt, [Rn]         @ Load halfword exclusive
LDREXD Rt, Rt2, [Rn]   @ Load doubleword exclusive (64-bit atomic)

/* STREX — Store Exclusive */
STREX  Rd, Rt, [Rn]    @ Store word; Rd=0 success, Rd=1 failure
STREXB Rd, Rt, [Rn]    @ Store byte exclusive
STREXH Rd, Rt, [Rn]    @ Store halfword exclusive
STREXD Rd, Rt, Rt2, [Rn] @ Store doubleword exclusive

/* CLREX — Clear Exclusive monitor */
CLREX                   @ Unconditionally clear exclusive monitor
                        @ Called on context switch, exception entry
```

### 4.3 Atomic Compare-and-Swap (CAS) Pattern

```assembly
/*
 * atomic_cmpxchg(ptr, old, new):
 *   if (*ptr == old) { *ptr = new; return old; }
 *   else { return *ptr; }
 *
 * ARM32 implementation:
 */
ENTRY(atomic_cmpxchg)
    /* r0 = ptr, r1 = old, r2 = new */
retry:
    LDREX  r3, [r0]         @ Load *ptr exclusively
    TEQ    r3, r1           @ Compare with old
    STREXEQ r4, r2, [r0]   @ If equal: try to store new
    BNE    done             @ Not equal: return current value
    TEQ    r4, #0           @ Check if STREX succeeded
    BNE    retry            @ STREX failed: retry
    DMB    ISH               @ Barrier: ensure new value visible
done:
    MOV    r0, r3           @ Return old value
    BX     lr
ENDPROC(atomic_cmpxchg)
```

### 4.4 Retry Loop Considerations

```
STREX failure cases:
  1. Another CPU did STREX to same cache line → fail, retry
  2. Cache line evicted between LDREX and STREX → fail
  3. Exception/interrupt between LDREX and STREX → fail (CLREX on entry)
  4. TLB/cache maintenance operation → may fail

Best Practices:
  - Keep LDREX/STREX region short (no branches, function calls, memory accesses)
  - No memory accesses between LDREX and STREX (can clear monitor inadvertently)
  - On failure: brief back-off before retry (avoid live-lock)
  - Use WFE (Wait For Event) in highly-contended spinlocks

WORST CASE: no progress if exception/IRQ fires repeatedly between LDREX/STREX
  - Real-time systems: use CONFIG_PREEMPT_RT which serializes LDREX/STREX
  - Normal Linux: acceptable — IRQ handlers are short
```

### 4.5 LDREXD/STREXD — 64-bit Atomics on ARM32

```assembly
/*
 * ARM32 doesn't have a native 64-bit atomic instruction
 * LDREXD/STREXD provides atomic 64-bit load/store
 *
 * Critical: Rt and Rt2 must be even/odd pair (r0,r1 or r2,r3 etc.)
 */

/* Atomic 64-bit read */
LDREXD r0, r1, [r2]    @ r0=low32, r1=high32, atomically
CLREX                   @ Don't need exclusive anymore after pure read

/* Atomic 64-bit CAS */
retry64:
    LDREXD r0, r1, [r4]    @ Load 64-bit exclusive
    /* compare r0:r1 with expected */
    TEQ    r0, r2
    TEQEQ  r1, r3
    BNE    no_match
    STREXD r5, r6, r7, [r4] @ Store new value (r6:r7)
    TEQS   r5, #0
    BNE    retry64
no_match:
    DMB    ISH
```

---

## 5. Atomic Operations in ARM32

### 5.1 Add and Return

```assembly
/*
 * atomic_add_return(delta, v):
 *   v->counter += delta; return v->counter;
 *
 * arch/arm/include/asm/atomic.h
 */
static inline int atomic_add_return(int delta, atomic_t *v)
{
    unsigned long tmp;
    int result;

    __asm__ __volatile__(
    "@ atomic_add_return\n"
"1: ldrex   %0, [%3]\n"     /* result = *v */
"   add     %0, %0, %4\n"   /* result += delta */
"   strex   %1, %0, [%3]\n" /* try *v = result */
"   teq     %1, #0\n"       /* check strex success */
"   bne     1b"             /* retry if failed */
    : "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
    : "r" (&v->counter), "Ir" (delta)
    : "cc");

    return result;
}
/* Note: No DMB here — caller adds barrier if needed (smp_mb__before_atomic) */
```

### 5.2 Test and Set (Spinlock Core)

```assembly
/*
 * arch_spin_lock() — ARM32 ticket spinlock
 * Uses LDREX/STREX to increment ticket counter atomically
 */

/* Ticket spinlock structure */
typedef struct {
    u16 next;   /* next ticket to be served */
    u16 owner;  /* current ticket being served */
} arch_spinlock_t;

void arch_spin_lock(arch_spinlock_t *lock)
{
    unsigned long tmp, newval;
    arch_spinlock_t lockval;

    __asm__ __volatile__(
    /* Get ticket: atomically increment lock->next, remember our ticket */
    "@ arch_spin_lock\n"
"1: ldrex   %0, [%3]\n"                 /* lockval = *lock (32-bit: next<<16|owner) */
"   add     %1, %0, %4\n"               /* newval = lockval + (1<<TICKET_SHIFT) */
"   strex   %2, %1, [%3]\n"             /* try *lock = newval */
"   teq     %2, #0\n"
"   bne     1b\n"                        /* retry if strex failed */
"   dmb     ish\n"                       /* barrier: ensure lock acquired before critical section */
    /* Spin until owner == our ticket */
"2: sev\n"                               /* send event (WFE optimization) */
"   ldrex   %2, [%3]\n"                 /* read current lock */
"   eor     %1, %0, %2\n"               /* compare next == owner ? */
"   ands    %2, %1, %4\n"
"   wfene\n"                             /* sleep if not our turn */
"   bne     2b\n"
"   dmb     ish"                         /* barrier: ensure lock visible before use */
    : "=&r" (lockval), "=&r" (newval), "=&r" (tmp)
    : "r" (lock), "I" (1 << TICKET_SHIFT)
    : "cc");
}
```

---

## 6. Linux Kernel Memory Barrier API

### 6.1 Complete Barrier Reference

```c
/* include/asm-generic/barrier.h + arch/arm/include/asm/barrier.h */

/* Full barriers (both load and store) */
mb()         /* Full memory barrier — all memory ops ordered */
             /* ARM: DMB SY (SMP) */
rmb()        /* Read memory barrier — load ordering */
             /* ARM: DMB ISH */
wmb()        /* Write memory barrier — store ordering */
             /* ARM: DMB ISHST */

/* SMP-only barriers (no-op on UP) */
smp_mb()     /* SMP full barrier */
             /* ARM SMP: DMB ISH, ARM UP: barrier() (compiler fence) */
smp_rmb()    /* SMP read barrier */
smp_wmb()    /* SMP write barrier */

/* Compiler-only fence (no CPU instruction) */
barrier()    /* Prevents compiler from reordering around this point */
             /* asm volatile("" ::: "memory") */

/* Acquire/Release semantics (C11 / Linux 4.x+) */
smp_load_acquire(ptr)   /* Load + load-acquire barrier (LoadLoad + LoadStore) */
smp_store_release(ptr, val)  /* Store-release barrier (StoreStore + LoadStore) */

/* Once semantics (no tearing, single read/write) */
READ_ONCE(x)    /* Exactly one load, no compiler optimization */
WRITE_ONCE(x, v)  /* Exactly one store, no compiler optimization */
```

### 6.2 smp_load_acquire / smp_store_release

```c
/*
 * Acquire/Release pattern — most efficient for producer/consumer:
 * Cheaper than full smp_mb() (only one-directional barrier)
 */

/* Producer (CPU0): */
WRITE_ONCE(buf[0], 42);         /* Write data */
WRITE_ONCE(buf[1], 99);
smp_store_release(&ready, 1);   /* Release barrier: all stores before this
                                   are visible before 'ready=1' is visible */

/* Consumer (CPU1): */
while (!smp_load_acquire(&ready)) { /* Acquire barrier: once we see ready=1,
    cpu_relax();                         all stores before release are visible */
}
val0 = READ_ONCE(buf[0]);       /* Guaranteed to be 42 */
val1 = READ_ONCE(buf[1]);       /* Guaranteed to be 99 */

/*
 * ARM32 implementation:
 * smp_store_release: stl (ARMv8) or str + dmb ishst (ARMv7)
 * smp_load_acquire: lda (ARMv8) or dmb ish + ldr (ARMv7)
 *
 * ARMv7 (ARM32): uses DMB-based emulation (no dedicated stl/lda)
 */
#define smp_store_release(p, v)          \
do {                                     \
    smp_mb();                            \
    WRITE_ONCE(*p, v);                   \
} while (0)

#define smp_load_acquire(p)              \
({                                       \
    typeof(*p) ___p1 = READ_ONCE(*p);   \
    smp_mb();                            \
    ___p1;                               \
})
```

### 6.3 Barrier in Device Drivers

```c
/* MMIO read/write ordering */

/* writel_relaxed(): store to device register, no barrier */
writel_relaxed(value, reg);

/* writel(): store + DMB before (ensures prior writes visible first) */
writel(value, reg);

/* readl_relaxed(): load from device register, no barrier */
u32 val = readl_relaxed(reg);

/* readl(): load + DMB after (ensures this load completes before next) */
u32 val = readl(reg);

/* Typical DMA trigger sequence: */
void trigger_dma(struct my_dev *dev, dma_addr_t buf, u32 size)
{
    /* 1. Write DMA address */
    writel_relaxed(buf, dev->base + DMA_ADDR_REG);
    writel_relaxed(size, dev->base + DMA_LEN_REG);

    /* 2. Barrier: ensure address/length written before DMA start */
    wmb();   /* or dma_wmb() for DMA-specific barrier */

    /* 3. Trigger DMA */
    writel(DMA_START, dev->base + DMA_CTRL_REG);
}
```

### 6.4 dma_wmb() vs wmb()

```c
/*
 * dma_wmb(): Write barrier for DMA operations
 * On ARM32 SMP with hardware-coherent DMA: DMB OSHST (Outer Shareable Store)
 * On ARM32 SMP without coherent DMA:       DMB SY (same as wmb())
 *
 * dma_rmb(): Read barrier after DMA completes
 * On ARM32: DMB OSH (Outer Shareable)
 *
 * These are cheaper than full wmb() on systems with hardware coherent DMA
 * because they only need to synchronize with outer shareable domain
 */

/* After DMA completion (consumer side): */
/* DMA wrote to buffer, now CPU reads it */
wait_for_dma_complete(dev);
dma_rmb();                              /* ensure DMA write visible to CPU */
result = READ_ONCE(dev->dma_buf[0]);   /* now safe to read */
```

---

## 7. Linux Atomic Operations API

### 7.1 atomic_t Operations

```c
/* include/linux/atomic.h */

/* Basic operations */
atomic_read(v)             /* READ_ONCE(v->counter) */
atomic_set(v, i)           /* WRITE_ONCE(v->counter, i) */

/* Arithmetic (return void) */
atomic_add(i, v)           /* v->counter += i */
atomic_sub(i, v)           /* v->counter -= i */
atomic_inc(v)              /* v->counter++ */
atomic_dec(v)              /* v->counter-- */

/* Arithmetic + return new value */
atomic_add_return(i, v)    /* v->counter += i; return counter */
atomic_sub_return(i, v)
atomic_inc_return(v)
atomic_dec_return(v)
atomic_fetch_add(i, v)     /* return old value, then add */

/* Test operations */
atomic_dec_and_test(v)     /* --v->counter == 0? */
atomic_inc_and_test(v)     /* ++v->counter == 0? */
atomic_sub_and_test(i, v)

/* Compare and exchange */
atomic_cmpxchg(v, old, new) /* if *v==old: *v=new, return old */
atomic_xchg(v, new)         /* *v = new, return old *v */

/* Bitwise */
atomic_or(i, v)
atomic_and(i, v)
atomic_andnot(i, v)

/* 64-bit variants */
atomic64_read(v)
atomic64_set(v, i)
atomic64_add(i, v)
atomic64_cmpxchg(v, old, new)
/* ARM32: uses LDREXD/STREXD pair */
```

### 7.2 Atomic Ordering Guarantees

```c
/*
 * Linux atomic operations and memory ordering:
 *
 * Without explicit barriers: atomic ops provide atomicity but NOT ordering
 * With smp_mb__before_atomic() / smp_mb__after_atomic(): add barrier
 */

/* Example: reference counting */
kref_put(&obj->refcount, obj_release);
/* kref_put calls atomic_dec_and_test() — returns 1 when counter reaches 0 */
/* For release: needs barrier BEFORE decrement (ensure all use complete) */
/* For acquire: needs barrier AFTER test (ensure release body executes after) */

/* How kref handles it: */
static inline void kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
    /* Paired with barrier in kref_get: */
    if (refcount_dec_and_test(&kref->refcount)) {
        /* smp_mb implicit here (from refcount_dec_and_test implementation) */
        release(kref);
    }
}

/* refcount_dec_and_test() on ARM32: */
/* LDREX/STREX + DMB ISH (full barrier inside) */
```

---

## 8. Spinlocks and Mutexes — ARM32 Implementation

### 8.1 Ticket Spinlock (ARM32 Linux)

```
ARM32 Linux uses TICKET spinlocks (fair FIFO ordering):

lock->next: Next ticket to be assigned
lock->owner: Currently-serving ticket

Ticket 0: Thread A arrives → takes ticket 0, next becomes 1
Ticket 1: Thread B arrives → takes ticket 1, next becomes 2
Thread A spins if owner==0: yes, A owns lock → enters critical section
Thread B spins if owner==1: no, waiting for A to finish
Thread A exits: owner becomes 1
Thread B: owner==1, B's ticket, B enters critical section
→ FAIR: threads served in arrival order (no starvation)
```

```c
/* arch/arm/include/asm/spinlock_types.h */
typedef struct {
    union {
        u32 slock;
        struct __raw_tickets {
#ifdef __ARMEB__
            u16 next;
            u16 owner;
#else
            u16 owner;
            u16 next;
#endif
        } tickets;
    };
} arch_spinlock_t;

/* arch/arm/include/asm/spinlock.h */
static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    smp_mb();   /* Barrier: ensure critical section writes visible before unlock */
    lock->tickets.owner++;
    dsb_sev();  /* DSB + SEV: wake up waiting CPUs (WFE optimization) */
}
```

### 8.2 Read-Write Spinlock

```assembly
/* arch/arm/include/asm/spinlock.h */
/*
 * rwlock: 32-bit word
 *   0x00000000 = unlocked
 *   0x80000000 = write-locked (1 writer)
 *   0x00000001+ = read-locked (count of readers)
 */

ENTRY(arch_write_lock)
    /* Atomic CAS: 0 → 0x80000000 */
1:  LDREX  r2, [r0]
    TEQ    r2, #0           @ Is unlocked?
    WFENE                   @ If not: wait for event (WFE)
    BNE    1b               @ Retry
    MOV    r2, #0x80000000
    STREX  r1, r2, [r0]    @ Try write-lock
    TEQ    r1, #0
    BNE    1b               @ STREX failed: retry
    DMB    ISH
    BX     lr

ENTRY(arch_read_lock)
    /* Atomic increment: non-negative values → add 1 */
1:  LDREX  r2, [r0]
    ADDS   r2, r2, #1       @ Increment; N flag if was negative (write-locked)
    WFEMI                   @ Wait if write-locked (N flag set)
    BMI    1b
    STREX  r1, r2, [r0]
    TEQ    r1, #0
    BNE    1b
    DMB    ISH
    BX     lr
```

---

## 9. Memory Types and Barrier Requirements

### 9.1 When Barriers Are Required by Memory Type

```
Strongly-Ordered memory:
  - Accesses never reordered by CPU
  - Accesses never buffered in write buffer
  - No explicit DMB/DSB needed for ordering BETWEEN Strongly-Ordered accesses
  - BUT: barrier needed to order Strongly-Ordered vs Normal memory

Device memory:
  - Accesses to same device not reordered
  - Multiple device accesses may be gathered (merged)
  - Barrier needed to order Device vs Normal memory
  - Barrier needed to prevent read-write merging if problematic

Normal memory:
  - Fully reorderable (most permissive)
  - All barriers apply: DMB/DSB enforce ordering

Most common rule of thumb:
  Peripheral register access (Device/SO): use writel() / readl()
    → These include barriers internally
  Shared memory (Normal): use smp_mb() / READ_ONCE / WRITE_ONCE
  DMA buffers (Normal NC): use dma_wmb() / dma_rmb()
```

### 9.2 Barrier Decision Tree

```
Q: Do I need a barrier here?

    Are you sharing data between CPUs?
    ├── YES → Is it via spinlock/mutex?
    │         ├── YES → No extra barrier needed (lock/unlock includes barrier)
    │         └── NO  → Need smp_mb() / smp_load_acquire / smp_store_release
    └── NO  → Are you sharing with DMA/hardware?
              ├── YES → Need dma_wmb() before trigger, dma_rmb() after complete
              └── NO  → Just use barrier() (compiler fence) if needed

    Are you writing CP15 registers?
    └── YES → DSB before + ISB after (always)

    Are you doing TLB invalidation?
    └── YES → DSB before TLBI + DSB after TLBI

    Are you doing cache maintenance?
    └── YES → DSB after maintenance ops
```

---

## 10. Common Barrier Bugs

### 10.1 Missing Barrier in Lock-Free Queue

```c
/* BUG: Lock-free ring buffer without barriers */
struct ring_buf {
    volatile u32 *data;
    volatile u32 head;
    volatile u32 tail;
};

/* Producer (CPU0): */
void enqueue_bug(struct ring_buf *rb, u32 val) {
    u32 pos = rb->tail;
    rb->data[pos] = val;       /* Write data */
    rb->tail = pos + 1;        /* Update tail — may be seen before data! */
    /* BUG: CPU/compiler may reorder tail update before data write */
}

/* Consumer (CPU1): */
u32 dequeue_bug(struct ring_buf *rb) {
    if (rb->head == rb->tail) return 0;  /* empty */
    u32 pos = rb->head;
    u32 val = rb->data[pos];    /* May read stale data! */
    rb->head = pos + 1;
    return val;
}

/* FIX: Use barriers + READ_ONCE/WRITE_ONCE */
void enqueue_fix(struct ring_buf *rb, u32 val) {
    u32 pos = rb->tail;
    WRITE_ONCE(rb->data[pos], val);   /* Compiler fence on write */
    smp_store_release(&rb->tail, pos + 1);  /* Release: data visible before tail */
}

u32 dequeue_fix(struct ring_buf *rb) {
    if (smp_load_acquire(&rb->head) == READ_ONCE(rb->tail)) return 0;
    u32 pos = rb->head;
    u32 val = READ_ONCE(rb->data[pos]);  /* Safe: after acquire */
    smp_store_release(&rb->head, pos + 1);
    return val;
}
```

### 10.2 Missing DSB Before MMU Enable

```assembly
/* BUG: Enable MMU without DSB */
MOV  r0, #(SCTLR_M | SCTLR_C | SCTLR_I)
MCR  p15, 0, r0, c1, c0, 0   @ Enable MMU
/* BUG: No DSB before! Page tables might not be visible to PTW */

/* FIX: */
MOV  r0, #(SCTLR_M | SCTLR_C | SCTLR_I)
DSB                            @ Ensure page table writes complete
MCR  p15, 0, r0, c1, c0, 0   @ Enable MMU
ISB                            @ Flush pipeline
```

### 10.3 Reordering of Interrupt Flag and Data

```c
/* BUG: Driver interrupt handler reads stale data */
static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    struct my_dev *dev = dev_id;
    /* DMA engine set complete_flag = 1 (DMA write) */
    /* But: DMA data writes to dma_buf might not be visible yet! */
    if (READ_ONCE(dev->complete_flag)) {
        process_data(dev->dma_buf);  /* BUG: dma_buf may have stale data */
    }
    return IRQ_HANDLED;
}

/* FIX: Barrier between flag and data read */
static irqreturn_t my_irq_handler_fix(int irq, void *dev_id)
{
    struct my_dev *dev = dev_id;
    if (READ_ONCE(dev->complete_flag)) {
        dma_rmb();                      /* Ensure DMA writes to dma_buf visible */
        process_data(dev->dma_buf);     /* Now safe */
    }
    return IRQ_HANDLED;
}
```

### 10.4 Double-Checked Locking (Classic Bug)

```c
/* BUG: Double-checked locking without barriers (pre-C11 pattern) */
struct singleton *get_instance_bug(void)
{
    if (!instance) {                 /* CHECK 1: may see partially-init object */
        spin_lock(&lock);
        if (!instance) {             /* CHECK 2 */
            instance = kmalloc(sizeof(*instance), GFP_KERNEL);
            init_instance(instance); /* init may not be visible after pointer write */
        }
        spin_unlock(&lock);
    }
    return instance;
}
/* BUG: CPU0 writes instance pointer, CPU1 sees non-NULL instance
        but CPU0's init writes may not yet be visible to CPU1 */

/* FIX: Use smp_store_release / smp_load_acquire */
struct singleton *get_instance_fix(void)
{
    struct singleton *inst = smp_load_acquire(&instance);  /* Acquire */
    if (!inst) {
        spin_lock(&lock);
        inst = READ_ONCE(instance);
        if (!inst) {
            inst = kmalloc(sizeof(*instance), GFP_KERNEL);
            init_instance(inst);
            smp_store_release(&instance, inst);  /* Release: init done before publish */
        }
        spin_unlock(&lock);
    }
    return inst;
}
```

---

## 11. Lock-Free Data Structures on ARM32

### 11.1 RCU (Read-Copy-Update) on ARM32

```c
/*
 * RCU: Most efficient reader path (no locks, no barriers in critical section)
 * Relies on: reader cannot see partially-constructed object
 *
 * ARM32: rcu_read_lock() / rcu_read_unlock() are essentially
 *        preempt_disable() / preempt_enable() in non-preemptible kernels
 *
 * Writer uses smp_store_release() to publish new pointer:
 */

struct my_data *new_data;

/* Writer: */
new_data = kmalloc(sizeof(*new_data), GFP_KERNEL);
new_data->field = 42;          /* Initialize */
rcu_assign_pointer(my_global_ptr, new_data);
/* rcu_assign_pointer: smp_store_release(&my_global_ptr, new_data) */

synchronize_rcu();             /* Wait for all RCU readers to complete */
kfree(old_data);

/* Reader: */
rcu_read_lock();
struct my_data *p = rcu_dereference(my_global_ptr);
/* rcu_dereference: smp_load_acquire(&my_global_ptr) */
/* Guarantees: all writes to *p before rcu_assign_pointer are visible */
use_data(p->field);             /* Safe: field==42 guaranteed */
rcu_read_unlock();
```

---

## 12. Performance Considerations

### 12.1 Barrier Cost on ARM32

```
Measured on Cortex-A9 (1.5GHz):

barrier()    : ~1 ns (compiler fence, no instruction)
DMB ISH      : ~15 ns (10-20 cycles — flush write buffer, sync inner shareable)
DMB SY       : ~40 ns (flush write buffer + wait for outer shareable)
DSB ISH      : ~50 ns (wait for all pending operations)
DSB SY       : ~100-200 ns (full system synchronization)
ISB          : ~10 ns (pipeline flush, ~5-10 cycles)
LDREX/STREX  : ~10 ns uncontended, ~100 ns heavily contended

lock(spinlock) / unlock(spinlock):
  Uncontended: ~50 ns (LDREX/STREX + 2 × DMB)
  Contended (4 CPUs): ~500 ns average wait
```

### 12.2 Avoiding Unnecessary Barriers

```c
/* BAD: Barrier inside tight loop */
for (int i = 0; i < 1000000; i++) {
    smp_mb();              /* ~15ns × 1M = 15ms overhead! */
    process(data[i]);
}

/* BETTER: Barrier outside loop if data doesn't change during loop */
smp_mb();
for (int i = 0; i < 1000000; i++) {
    process(data[i]);
}
smp_mb();

/* BAD: Using full smp_mb() when acquire/release sufficient */
void publish(u64 *buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = compute(i);
    smp_mb();      /* Heavy: DMB ISH full */
    count = n;
}

/* BETTER: Use smp_store_release (cheaper on ARM64, same cost ARM32) */
void publish_fix(u64 *buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = compute(i);
    smp_store_release(&count, n);   /* Store-release: sufficient, clearer intent */
}
```

---

## Summary: Barrier Cheat Sheet

```
Operation                       Barrier Required
─────────────────────────────────────────────────────────────────
SMP shared flag + data          smp_store_release / smp_load_acquire
SMP spinlock unlock             smp_mb() [inside spin_unlock]
SMP atomic dec + test           smp_mb__before/after_atomic
DMA trigger                     dma_wmb() (wmb() if non-coherent)
DMA completion read             dma_rmb()
MMIO write (simple)             writel() [includes DMB internally]
MMIO sequence                   writel_relaxed() + wmb() + writel()
TLB invalidate                  DSB before + DSB after TLBI
Cache maintenance               DSB after
CP15 write (TTBR/SCTLR)        DSB before + MCR + ISB after
MMU enable                      DSB + MCR SCTLR + ISB
Context switch TTBR0 write      MCR CONTEXTIDR + ISB + MCR TTBR0 + ISB
Lock-free publish               WRITE_ONCE + smp_store_release
Lock-free consume               smp_load_acquire + READ_ONCE
```

---

**Cross-References:**
- Doc 04: DSB required around TLB invalidation
- Doc 05: Cache maintenance barrier requirements
- Doc 06: DSB/ISB in world switch (Monitor mode)
- Doc 09: Memory barriers in hypervisor context (Stage-2 table updates)

---
**End of Document 8**
