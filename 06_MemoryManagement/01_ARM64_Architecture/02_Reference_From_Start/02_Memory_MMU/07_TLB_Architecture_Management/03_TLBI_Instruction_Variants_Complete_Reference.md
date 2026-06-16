# TLBI Instruction Variants: Complete Reference

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. TLBI Instruction Syntax

```
TLBI <op>, <Xt>

<op> encodes:
  - What to invalidate (all, by VA, by ASID, etc.)
  - Which translation regime (EL1, EL2, EL3)
  - Scope (IS = Inner Shareable broadcast, or local)

<Xt>:
  Optional operand register containing VA or ASID for targeted invalidation
  Format varies by operation (see below)

General rule for operation name:
  [Range][VA/AS][Level][EL][Scope][EL qualifier]

Example: TLBI VAAE1IS
  VA = by VA
  A = all ASIDs
  E1 = EL1 regime
  IS = Inner Shareable (broadcast to all cores in ISH domain)
  → "Invalidate TLB entries by VA for all ASIDs, EL1, broadcast to ISH"
```

---

## 2. Key TLBI Variants

```
TLBI VMALLE1IS:
  Syntax: TLBI VMALLE1IS
  No operand register
  Invalidates: ALL TLB entries for EL1 and EL0 (current VMID)
  Scope: Inner Shareable broadcast (all CPUs in ISH domain)
  Use: Context switch with ASID rollover, full TLB flush, module unload
       Linux: flush_tlb_all() → TLBI VMALLE1IS + DSB ISH + ISB

TLBI VAE1IS, Xt:
  Operand: Xt[43:0] = VA[55:12] (VA >> 12, zero-extended to 44 bits)
           Xt[63:48] = ASID
  Invalidates: Entries for specified VA + ASID at EL1/EL0
  Scope: ISH broadcast
  Use: mprotect(), munmap() on specific VA range
       Linux: flush_tlb_page() → TLBI VAE1IS

TLBI ASIDE1IS, Xt:
  Operand: Xt[15:0] = ASID (if 16-bit ASID) or [7:0] for 8-bit
  Invalidates: ALL entries with matching ASID
  Scope: ISH broadcast
  Use: Process exit (flush all mappings for one process)
       (Actually Linux rarely uses this — uses ASID repurposing instead)

TLBI VAAE1IS, Xt:
  Operand: Xt[43:0] = VA >> 12
  Invalidates: Entries for specified VA, ALL ASIDs
  Scope: ISH broadcast
  Use: mprotect() on page that's shared (mapped in multiple processes)
       When you don't know which ASID to use
       Linux: flush_tlb_kernel_range() uses this for kernel VA operations

TLBI VALE1IS, Xt:
  Like VAE1IS but only invalidates "last level" (leaf) entries
  Does NOT invalidate intermediate page table entries (L0/L1/L2 table entries)
  Optimization: if you know only leaf PTE changed (permission change), not tables
  Linux: ptep_clear_flush() → TLBI VALE1IS (or VAE1IS depending on config)

TLBI VMALLE1:
  Like VMALLE1IS but local only (current CPU, no broadcast)
  Use: in uniprocessor context, or after already broadcasting via other means
  Also used in EL2 code where IS scope is not appropriate

TLBI ALLE1IS:
  Invalidates ALL EL1/EL0 entries across all VMIDs
  Use: rare — hypervisor tearing down all guests
```

---

## 3. TLBI Operand Register Format

```
For VA-based TLBI operations (VAE1IS, VALE1IS, VAAE1IS, VAALE1IS):

  Xt register:
  ┌────────────┬──────────────────────────┐
  │ ASID[63:48] │ VA[43:0] = VA_addr >> 12 │
  └────────────┴──────────────────────────┘

  VA[43:0]: The virtual address right-shifted by 12 (page number)
  For 48-bit VA: VA_addr[47:12] fits in [35:0] of the 44-bit VA field
  ASID[63:48]: The ASID for the target process

  Example:
    To invalidate VA=0x400000 for ASID=5:
    X0 = (5ULL << 48) | (0x400000 >> 12)
       = (5ULL << 48) | 0x400
    TLBI VAE1IS, X0

For ASID-based TLBI (ASIDE1IS):
  Xt[15:0] = ASID value
  Xt[47:16] = reserved (should be 0)
```

---

## 4. Mandatory DSB + ISB Sequence

```
TLBI instructions do NOT immediately take effect on all CPUs.
The invalidation is QUEUED — it takes time to propagate.

You MUST use DSB and ISB after TLBI:

Correct sequence:
  TLBI VAE1IS, Xt    // Broadcast invalidation (queued)
  DSB ISH            // Wait until ALL TLB invalidations complete
                     // DSB ensures: all CPUs have completed their TLB inval
                     // before any new memory accesses proceed
  ISB                // Flush pipeline: ensures subsequent instruction fetches
                     // use the new (post-invalidation) TLB state

Why DSB is needed:
  TLBI IS broadcasts to all CPUs in ISH domain.
  The TLBI itself is a barrier for the issuing CPU.
  But OTHER CPUs receiving the broadcast need time to process it.
  DSB ISH: waits until the broadcast has completed on ALL ISH CPUs.
  Without DSB: new code running on another CPU might still use stale TLB entry.

Why ISB is needed:
  After DSB, data memory is coherent.
  BUT: the CPU pipeline may have already fetched future instructions.
  Those fetched instructions might use VAs that need to be re-translated.
  ISB flushes the pipeline: ensures all instructions after ISB are freshly fetched
  using the updated (post-TLBI) translation state.

Linux implementation (arch/arm64/include/asm/tlbflush.h):
  #define __flush_tlb_one_user(addr) \
      do { \
          tlbi(vale1is, __TLBI_VADDR(addr, 0)); \
          dsb(ish); \
      } while (0)
  // ISB is implicitly present because after flush, the returned code
  // has an ISB in flush_tlb_range → __flush_tlb_range_nosync

Full flush example:
  static inline void flush_tlb_all(void) {
      dsb(ishst);          // Ensure all page table writes are visible
      tlbi(vmalle1is);     // Broadcast: invalidate all entries
      dsb(ish);            // Wait for invalidation to complete
      isb();               // Pipeline flush
  }
```

---

## 5. TLBI Range Operations (ARMv8.4, FEAT_TLBRANGE)

```
Problem with page-by-page TLBI:
  munmap(start, 1GB):
    1 GB / 4 KB = 262,144 pages → 262,144 TLBI VAE1IS instructions
    Each broadcast to all 16 cores → 4 million TLB invalidation messages
    Latency: 100–1000 µs for large munmap regions
    
  Solution: TLBI range operations (ARMv8.4 FEAT_TLBRANGE)
  
TLBI RVAAE1IS, Xt:  (Range-based VAE1IS)
  Xt format:
    [63:48] = ASID
    [47:46] = TG (Translation Granule: 0b00=4KB, 0b01=64KB, 0b10=16KB)
    [45:44] = SCALE (size scale factor)
    [43:39] = NUM (number of pages - 1 in the range, 5-bit)
    [38:0]  = BaseADDR = Base VA >> 12
    
    Range = BaseADDR to BaseADDR + (NUM+1) * 2^(5*SCALE+1)
    Maximum range: 8GB in one TLBI instruction

  Linux uses TLBI range ops when FEAT_TLBRANGE is present:
    Detection: ID_AA64ISAR0_EL1.TLB[27:24] or ID_AA64MMFR2_EL1 (varies)
    flush_tlb_range() with large ranges → auto-switches to RVAAE1IS
    Typical threshold: if range > 64 pages → use range TLBI
    if range <= 64 pages → use loop of VALE1IS (each single-page)
```

---

## 6. Linux TLB Flush API

```
Linux TLB flush functions (arch/arm64/include/asm/tlbflush.h):

flush_tlb_all():
  TLBI VMALLE1IS → DSB ISH → ISB
  Use: global TLB flush, ASID rollover, early boot

flush_tlb_mm(mm):
  TLBI ASIDE1IS, asid → DSB ISH
  Use: process exit (flush process's TLB entries by ASID)

flush_tlb_page(vma, addr):
  TLBI VAE1IS, {asid, addr>>12} → DSB ISH
  Use: page table entry changed for specific page

flush_tlb_range(vma, start, end):
  If FEAT_TLBRANGE available:
    TLBI RVAAE1IS for the range → DSB ISH
  Else:
    Loop: for each page in [start, end): TLBI VAE1IS
  Use: mprotect(), munmap() on a range

flush_tlb_kernel_range(start, end):
  TLBI VAAE1IS (all ASIDs, kernel VA)
  Use: kernel vmalloc range changes

local_flush_tlb_all():
  TLBI VMALLE1 (no IS — local CPU only)
  Use: early boot before SMP, or within per-CPU context
```

---

## 7. Interview Questions & Answers

**Q1: What is the difference between TLBI VMALLE1IS and TLBI VALE1IS, and when would you use each?**

**TLBI VMALLE1IS** invalidates ALL TLB entries for EL1 and EL0 across the entire Inner Shareable domain (all CPUs). It takes no operand — it's a complete flush. Use it for: ASID rollover (all translations stale), late boot MMU enable, or when you want to completely reset the TLB state. It's expensive because it invalidates everything, causing TLB cold-miss overhead until entries refill.

**TLBI VALE1IS** invalidates a SPECIFIC virtual address at the last level (leaf PTE) only. It does NOT invalidate intermediate page table entries (L0/L1/L2 table descriptor caches). Use it for targeted, fine-grained invalidation when only a leaf PTE changed — for example, a permission change via `mprotect()` that changes `AP[2:1]` or `UXN` in a single PTE. The "last level only" optimization is valid because when only a leaf PTE changes, the intermediate table entries (which point to the same sub-table) are still correct — only the final cached translation needs refreshing. If you change a TABLE descriptor (replace a table pointer), you'd need VAE1IS (which also invalidates cached table-walk entries).

---

## 8. Quick Reference

| TLBI Operation | Invalidates | Operand | Use case |
|---|---|---|---|
| VMALLE1IS | All EL1/EL0 entries | None | Full flush, ASID rollover |
| VAE1IS | VA + ASID specific | {ASID, VA>>12} | munmap, mprotect single page |
| VALE1IS | Last-level only, VA+ASID | {ASID, VA>>12} | Leaf PTE change only |
| VAAE1IS | VA, all ASIDs | {VA>>12} | Kernel VA changes, shared pages |
| ASIDE1IS | All entries for ASID | {ASID} | Process exit |
| RVAAE1IS | Range of VAs, all ASIDs | Encoded range | Large munmap/mprotect range |

| Scope suffix | Meaning |
|---|---|
| IS | Inner Shareable (broadcast to all CPUs in ISH domain) |
| (none) | Local CPU only |
