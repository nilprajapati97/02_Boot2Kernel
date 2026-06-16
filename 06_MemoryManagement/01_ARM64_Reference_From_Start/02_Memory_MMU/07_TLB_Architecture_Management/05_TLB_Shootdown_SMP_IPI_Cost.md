# TLB Shootdown: SMP IPI Cost and Performance Impact

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. What is TLB Shootdown?

```
TLB Shootdown: the process of invalidating TLB entries on MULTIPLE CPUs.

Why needed:
  ARM64 SMP system: 8, 16, or 128 CPU cores.
  Each core has its own private L1 iTLB, L1 dTLB.
  L2 TLB may be per-core or shared per-cluster.
  
  When a page table entry changes (mprotect, munmap, page migration):
    Only the CPU making the change knows about it immediately.
    Other CPUs may have the OLD (stale) translation in their L1/L2 TLBs.
    If they continue to use stale TLB entries:
      - Wrong PA for a VA that was remapped
      - Old permissions for a page that was made read-only
      - Access to a page that was unmapped (use-after-free)
    → Data corruption, security holes, or kernel panics
    
  TLB shootdown = ensure all cores invalidate the stale TLB entry.

Two approaches:
  1. ARM64 TLBI with IS (Inner Shareable) broadcast:
     Hardware interconnect broadcasts the invalidation request
     All CPUs in ISH domain process it automatically
     No software IPI needed!
     
  2. Software IPI (on architectures without HW broadcast):
     CPU that changes PTE sends an IPI to all other CPUs
     Each remote CPU executes TLBI locally (TLBI without IS)
     x86 uses this approach (invlpg is local-only)
     x86 TLB shootdown via APIC IPI is expensive
```

---

## 2. ARM64 TLBI IS: Hardware Broadcast (No IPI Needed)

```
ARM64 TLBI with IS suffix (Inner Shareable):
  TLBI VMALLE1IS
  TLBI VAE1IS, Xt
  TLBI RVAAE1IS, Xt (range, ARMv8.4)
  
  "IS" = Inner Shareable domain
  ARM interconnect (CCI, CCN, CMN) broadcasts the invalidation request
  to ALL CPU cores in the ISH domain — automatically, in hardware.
  
  Each receiving CPU processes the invalidation in parallel:
    CPU0 issues TLBI VAE1IS → interconnect fans out → CPUs 1..N process simultaneously
    Completion: DSB ISH waits until ALL cores have completed the invalidation

  Critical advantage vs x86:
    No software IPI required
    No interrupt handling overhead on remote CPUs
    No need for each CPU to enter the interrupt handler
    Latency: hardware coherency protocol, not software interrupt delivery
    
  Typical ARM64 TLB shootdown latency (TLBI VAE1IS + DSB ISH):
    4-core phone SoC: 1–3 µs
    16-core server: 2–8 µs
    128-core Altra/Graviton: 10–30 µs (more CPUs = longer broadcast)

  Comparison: x86 TLB shootdown via IPI:
    Each IPI: interrupt delivery + handler + INVLPG + ACK
    Per-CPU: ~1–5 µs
    16-core: ~16–80 µs (sequential or batched)
    ARM64 advantage: 2–10× faster TLB shootdown than x86 for typical server configs
```

---

## 3. mprotect() TLB Shootdown Analysis

```
mprotect(addr, len, PROT_READ) on 1000 pages:
  Process running on CPU0 with 3 other threads on CPU1/2/3

Sequence:
  1. CPU0 calls mprotect() → sys_mprotect() → mprotect_fixup()
  2. For each VMA in range:
     change_protection_range() → change_pte_range()
  3. For each PTE:
     ptep_modify_prot_start(): clears PTE valid bit (sets to 0b00)
     Update PTE: change AP[2]=1 (read-only)
     ptep_modify_prot_commit(): sets PTE valid bit again
     CPU0's own TLB: the store to PTE automatically invalidates CPU0's TLB
     (CPU0 might still cache old PTE in walk cache — needs TLBI)
  4. AFTER all PTEs updated:
     flush_tlb_range(vma, start, end)
     → TLBI RVAAE1IS (range) OR loop of TLBI VAE1IS (per page)
     → DSB ISH → ISB

  Result on CPU1/2/3:
    Receive the broadcast TLB invalidation
    Their L1/L2 TLBs for the affected VAs are cleared
    Next access to these pages: TLB miss → PTW reads new PTE → RO permission
    CPU1/2/3 did NOT have to stop or receive an interrupt!
    The invalidation is processed asynchronously by the interconnect protocol.

TLBI + DSB ensures:
  By the time mprotect() returns to user space:
  ALL CPUs have invalidated the old translations
  Write to the now-RO page on any CPU → permission fault
```

---

## 4. Context Switch TLB Cost: With vs Without ASID

```
Without ASID (hypothetical):
  Every context switch: flush ALL TLBs
  TLBI VMALLE1IS → DSB ISH → ISB
  Cost: ~2–10 µs on modern systems
  At 10,000 ctx/sec: 20–100 ms/sec wasted = 2–10% CPU overhead
  
  Plus: all threads on all CPUs lose their TLB warmth
  After flush: ALL threads suffer TLB cold misses
  Especially bad for workloads with many context switches (database, web server)

With ASID (ARM64 default):
  Context switch: just load new TTBR0_EL1 with new ASID + page table
  No TLBI needed!
  Cost: MSR TTBR0_EL1 + ISB = 1–5 cycles
  
  Old process's TLB entries remain in TLB (invisible, tagged with old ASID)
  New process's TLB entries from previous run also remain (tagged with new ASID)
  → TLB "warm" for new process if it ran recently!
  
  Benefit: same process context switches (switching back to same process):
    ASID reused → TLB entries still valid → immediate TLB hits
    First access after context switch: TLB HIT not MISS (if entry still present)
    Huge performance win for frequently-switching processes (task scheduler)

Numbers:
  System with 10,000 ctx/sec, 4KB pages, 48-bit VA:
    Without ASID: ~100 TLB misses per task × 10,000 ctx/sec = 1M extra TLB walks/sec
    With ASID: 0 TLB misses for context switch itself
    (only cold misses when process accesses NEW pages never visited before)
```

---

## 5. Batched TLB Invalidation in Linux

```
Linux optimizes TLB flushes by batching multiple invalidations:

mmu_gather (struct mmu_gather):
  Used during unmap operations (munmap, exit, fork CoW cleanup)
  Collects the set of pages and VA ranges that need TLB invalidation
  Batches the TLBI operations and defers until mmu_gather_flush()
  
  tlb_gather_mmu(tlb, mm)  → start batch
  unmap_page_range()       → collect pages to unmap, defer TLBI
  tlb_finish_mmu(tlb)      → flush_tlb_range() for batched VAs

Deferred TLB invalidation benefit:
  Without batching: 1000 mprotect pages → 1000 separate TLBI IS calls
  With batching: 1000 pages → 1 range TLBI IS call (or few range ops)
  Reduces interconnect broadcast traffic 1000×
  DSB ISH called once at the end instead of 1000 times

RCU-based deferred TLB flush:
  Some kernel code uses RCU to defer TLB invalidation:
    Mark PTE invalid → wait for RCU grace period → free physical page
    All CPUs must have gone through a quiescent state (no stale TLB hit possible)
    after the grace period → safe to free without explicit TLBI
  
  Used for: page reclaim, SLUB, page cache eviction
  Reduces TLBI overhead for kernel-internal memory management
```

---

## 6. Interview Questions & Answers

**Q1: On ARM64, when a process calls munmap(), how does the kernel ensure no other CPU thread continues to access the unmapped pages?**

1. The kernel (on the calling CPU) iterates through the VA range in the VMA, clearing each PTE (setting it to 0x0 — invalid). These stores to page tables go through the cache coherency protocol, so all CPUs will eventually see the updated PTEs.

2. However, other CPUs may have the OLD translations cached in their L1/L2 TLBs. With the PTE cleared, new TLB misses will correctly fault (invalid descriptor), but EXISTING stale TLB entries bypass the page table lookup entirely.

3. The kernel calls `flush_tlb_range()` → `TLBI RVAAE1IS` (or per-page `TLBI VAE1IS`) to invalidate TLB entries for the unmapped VA range. The IS suffix broadcasts this invalidation to all CPUs in the Inner Shareable domain — hardware handles the broadcast, no software IPI needed.

4. `DSB ISH` waits until ALL CPUs have completed the TLB invalidation.

5. After `DSB ISH` completes, the kernel can safely free the physical pages (decrement refcount, return to buddy allocator). Any CPU that tries to access the old VA now gets a TLB miss → PTW finds invalid PTE → page fault → SIGSEGV.

This sequence guarantees there's no window where a CPU could use a stale TLB entry to access a freed page (use-after-free via stale TLB).

---

## 7. Quick Reference

| Operation | TLBI instruction | Scope | Cost |
|---|---|---|---|
| munmap single page | VAE1IS | ISH | ~1–5 µs |
| munmap large range | RVAAE1IS | ISH | ~2–10 µs |
| mprotect page | VALE1IS | ISH | ~1–3 µs |
| Context switch | None (ASID) | — | <5 cycles |
| Process exit | ASIDE1IS | ISH | ~1–5 µs |
| ASID rollover | VMALLE1IS | ISH | ~2–30 µs (per core count) |

| Architecture | TLB shootdown mechanism | Latency (16 cores) |
|---|---|---|
| ARM64 | TLBI IS (hardware broadcast) | ~2–8 µs |
| x86 | Software IPI (INVLPG remote) | ~16–80 µs |
