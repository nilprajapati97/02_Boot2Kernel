# DMB Instruction: All Variants and Usage

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
DMB (Data Memory Barrier): prevents memory accesses from being reordered
ACROSS the barrier point, but DOES NOT stall the pipeline like DSB.

DMB guarantees:
  All memory accesses (of specified type) BEFORE the DMB complete
  BEFORE any memory accesses (of specified type) AFTER the DMB
  
  "Complete" means: VISIBLE to other observers in the shareability domain
  
  DMB does NOT:
    - Stall the execution pipeline (instructions can issue, just not reach memory)
    - Ensure instruction side effects (ISB needed for that)
    - Enforce DMA device completion (DSB + device register read needed)
    - Guarantee cache maintenance completion (that's DSB's job)

DMB vs DSB:
  DMB: "Memory accesses before me are ordered before those after me"
       Pipeline can CONTINUE (next instructions execute speculatively)
  DSB: "ALL previous memory accesses have COMPLETED before I complete"
       Pipeline STALLS until memory system quiesces
       
  Cost: DMB ≈ 10–50 ns (depends on store buffer drain)
        DSB ≈ 30–100+ ns (pipeline stall, waits for memory system)
```

---

## 2. DMB Variants: Shareability Domain

```
DMB <type><domain>

Shareability domain (domain part):
  SY  = Full System (all observers: all CPUs + all DMA masters)
  ISH = Inner Shareable (all CPUs in the Inner Shareable domain)
  OSH = Outer Shareable (all CPUs + some DMA masters in Outer Shareable domain)
  NSH = Non-Shareable (only this CPU; almost never used)

ARM64 system topology example (Qualcomm Snapdragon):
  
  CPU cluster 0: [Cortex-X2, A710×3]  ←── Inner Shareable domain
  CPU cluster 1: [A510×4]              ←── Inner Shareable domain
  ┌─────────────────────────────────────────────────────────┐
  │           SoC Interconnect (CCI-650)                    │
  │  Inner Shareable: all CPU clusters                      │
  │  Outer Shareable: CPUs + DSPs + GPU (same coherency)    │
  │  Full System (SY): CPUs + DSPs + GPU + PCIe + USB + ...  │
  └─────────────────────────────────────────────────────────┘
  
  DMB ISH: ordering for CPU-to-CPU communication on this SoC
  DMB OSH: ordering for CPU-to-GPU shared memory
  DMB SY:  ordering for CPU-to-PCIe/DMA device communication
  
Linux default for smp_mb()/smp_rmb()/smp_wmb():
  Uses DMB ISH (Inner Shareable)
  Rationale: Linux SMP only needs CPU-to-CPU ordering
  
Linux mb()/rmb()/wmb() (non-SMP variants):
  Uses DSB SY (Full System barrier)
  Used for: MMIO and device register ordering
```

---

## 3. DMB Variants: Access Type

```
Access type (type part):

No suffix (full): SY (load+store barrier)
  DMB SY  → both loads and stores ordered
  Covers: Load-Load, Load-Store, Store-Load, Store-Store
  Most expensive variant

LD (load) suffix:
  DMB ISHLD → only LOADS ordered
  Guarantees: all LOADS before DMB are visible before LOADS after DMB
  Does NOT order stores before the DMB relative to after
  Use case: Consumer side of producer-consumer pattern
  
ST (store) suffix:
  DMB ISHST → only STORES ordered
  Guarantees: all STORES before DMB are committed before STORES after DMB
  Does NOT order loads before DMB relative to after
  Use case: Producer side of producer-consumer pattern

ARM64 encoding:
  DMB ISH   = 0xD5033BBF (CRn=0x3, CRm=0xB, op2=0x5)
  DMB ISHLD = 0xD5033ABF (CRn=0x3, CRm=0xA, op2=0x5)
  DMB ISHST = 0xD5033ABF — wait, let me be precise:
  
  DMB instruction encoding: SYS #0, C3, C<CRm>, #5
    CRm = 0b1111 (SY), 0b1110 (ST), 0b1101 (LD) for Full System
    CRm = 0b1011 (ISH), 0b1010 (ISHST), 0b1001 (ISHLD) for Inner Shareable
    CRm = 0b0111 (OSH), 0b0110 (OSHST), 0b0101 (OSHLD) for Outer Shareable
    CRm = 0b0011 (NSH), 0b0010 (NSHST), 0b0001 (NSHLD) for Non-Shareable

Choosing the right variant:
  CPU-to-CPU, normal RAM:
    Producer: smp_wmb() = DMB ISHST  (only store ordering needed)
    Consumer: smp_rmb() = DMB ISHLD  (only load ordering needed)
    Both: smp_mb()      = DMB ISH    (full bidirectional)
    
  CPU-to-DMA device:
    Use wmb() = DSB ST (system-level store ordering)
    
  MMIO register sequence:
    Use DSB SY between register accesses for guaranteed device ordering
```

---

## 4. Practical Code Examples

```
Example 1: Message passing (most common pattern)

PRODUCER (CPU0):
  // Write payload data
  msg.data1 = value1;        // STR w1, [x0, #offset1]
  msg.data2 = value2;        // STR w2, [x0, #offset2]
  smp_wmb();                 // DMB ISHST — all stores above visible before flag
  msg.ready = 1;             // STR w3, [x0, #ready_offset]
  
CONSUMER (CPU1):
  while (!READ_ONCE(msg.ready));  // LDR + branch
  smp_rmb();                      // DMB ISHLD — flag load ordered before data loads
  val1 = READ_ONCE(msg.data1);    // LDR
  val2 = READ_ONCE(msg.data2);    // LDR
  
Result: CPU1 is guaranteed to see both data1 and data2 valid
  
Example 2: Ring buffer producer (io_uring style)

void ring_produce(struct ring *r, struct entry *e) {
    unsigned prod_idx = r->prod_idx;        // local copy
    
    // Write entry
    r->entries[prod_idx] = *e;              // STR (multi-word)
    
    // Barrier: entry must be visible before idx update
    smp_wmb();                              // DMB ISHST
    
    // Update producer index (consumer spins on this)
    WRITE_ONCE(r->prod_idx, prod_idx + 1); // STR (single atomic)
}

void ring_consume(struct ring *r, struct entry *out) {
    unsigned cons_idx = r->cons_idx;
    
    // Wait for new entry
    while (READ_ONCE(r->prod_idx) == cons_idx);  // LDR + branch
    
    // Barrier: idx load must complete before entry reads
    smp_rmb();                              // DMB ISHLD
    
    // Read entry
    *out = r->entries[cons_idx];            // LDR (multi-word)
    
    // Barrier: entry reads must complete before advancing cons_idx
    smp_mb();                               // DMB ISH
    WRITE_ONCE(r->cons_idx, cons_idx + 1);
}

Example 3: MMIO device write sequence
  // Wrong:
  writel(DMA_SRC, src_addr);
  writel(DMA_DST, dst_addr);
  writel(DMA_LEN, length);
  writel(DMA_START, 1);  // START may reach device before ADDR registers!
  
  // Correct: use wmb() between register writes and the trigger
  writel(DMA_SRC, src_addr);
  writel(DMA_DST, dst_addr);
  writel(DMA_LEN, length);
  wmb();                 // DSB ST — all writes above complete before START
  writel(DMA_START, 1);
```

---

## 5. DMB Interaction with Cache Maintenance

```
Common mistake: using DMB for cache maintenance ordering

WRONG:
  DC CIVAC, x0  // clean+invalidate cache line
  DMB ISH       // barrier
  // Now assume device can read from DRAM... NOT GUARANTEED!

Why wrong: DC CIVAC is a MEMORY ACCESS (cache maintenance instruction)
DMB orders memory accesses, but DC CIVAC may not have COMPLETED yet
DMB ensures ORDERING but not COMPLETION

CORRECT:
  DC CIVAC, x0  // clean+invalidate
  DSB ISH       // stall until DC CIVAC fully completes (reaches DRAM)
  // Now device can read from DRAM ← guaranteed

Rule: cache maintenance → use DSB, not DMB
Rule: general memory ordering between CPUs → use DMB ISH

For DMA setup:
  dma_map_single() implementation on ARM64:
    DC CIVAC (flush each cache line in the buffer)
    DSB ISH  (wait for all DC CIVAC to complete)
    // Now DMA controller can access the buffer
    
  Not DMB ISH! Must be DSB to ensure actual completion to PoC
```

---

## 6. Performance Considerations

```
DMB ISH performance on ARM64:

Cortex-A78, A710, X2:
  DMB ISH: approximately 10–50 cycles
  (Drains store buffer to L2 cache, serializes outstanding loads)
  
  In a tight loop: DMB ISH can be the DOMINANT cost!
  
Example: high-frequency counter update (WRONG - too many barriers):
  for (i = 0; i < 1000000; i++) {
      WRITE_ONCE(*counter, i);
      smp_mb();              // DMB ISH: 50 cycles × 1M = 50M cycles = ~20ms!
  }
  
Better: batch updates, use acquire/release instead of full barriers:
  for (i = 0; i < 1000000; i++) {
      smp_store_release(counter, i);  // STLR: 1 instruction, lighter weight
  }
  
Even better: use per-CPU counters, aggregate only when needed

ARM FEAT_LRCPC (Load-Release Register from Register):
  ARMv8.3: LDAPR (weaker acquire semantics)
  Cheaper than LDAR on some microarchitectures
  Linux uses LDAPR via smp_cond_load_acquire() when available
  
One-sided barriers (prefer over full DMB):
  smp_store_release + smp_load_acquire: half the cost of two DMB ISH
  For producer-consumer: asymmetric barriers sufficient
  Full smp_mb(): only when Store-Load ordering is needed (Dekker's)
```

---

## 7. Interview Questions & Answers

**Q1: What is the difference between DMB ISH and DMB SY, and when would you use each?**

`DMB ISH` (Inner Shareable) orders memory operations among all participants in the Inner Shareable domain, which on most ARM64 SoCs includes all CPU cores. `DMB SY` (Full System) extends this ordering to ALL masters including DMA controllers, GPU compute units, PCIe devices, and other non-CPU agents connected via the system interconnect.

Use `DMB ISH` (via `smp_mb()`) for CPU-to-CPU communication over normal RAM — this is the common case in SMP kernel code. Use `DMB SY` (via `mb()`) or DSB SY when communicating with DMA devices or MMIO regions where a device might be an observer. In practice, `writel()`/`readl()` and `dma_map_single()` in Linux already include the appropriate full-system barriers, so you rarely call `mb()` directly.

---

## 8. Quick Reference

| Instruction | Domain | Types | Linux Equivalent |
|---|---|---|---|
| DMB ISH | Inner Shareable | Load+Store | smp_mb() |
| DMB ISHLD | Inner Shareable | Load only | smp_rmb() |
| DMB ISHST | Inner Shareable | Store only | smp_wmb() |
| DMB OSH | Outer Shareable | Load+Store | (rare, DSP/GPU) |
| DMB SY | Full System | Load+Store | mb() |
| DSB ISH | Inner Shareable | All+pipeline | cache maintenance |
| DSB SY | Full System | All+pipeline | mb() / cache maint |

| Pattern | Producer | Consumer |
|---|---|---|
| Write data then flag | smp_wmb() = DMB ISHST | smp_rmb() = DMB ISHLD |
| Mutual exclusion | smp_mb() = DMB ISH | smp_mb() = DMB ISH |
| Acquire/Release | smp_store_release() = STLR | smp_load_acquire() = LDAR |
| MMIO write ordering | wmb() = DSB ST | rmb() = DSB LD |
