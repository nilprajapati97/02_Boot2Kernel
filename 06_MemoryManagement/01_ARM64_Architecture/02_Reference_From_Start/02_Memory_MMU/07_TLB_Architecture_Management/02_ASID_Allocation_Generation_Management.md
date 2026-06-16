# ASID: Address Space Identifiers — Allocation and Management

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Why ASIDs Exist

```
Without ASIDs:
  On every context switch (process A → process B):
    TLB contains process A's translations (VA→PA mappings)
    Process B uses the same VA ranges (e.g., 0x400000 for .text)
    TLB still has A's mapping for 0x400000 → would return A's PA
    WRONG! Must flush the entire TLB before running process B.
    
    TLB flush on every context switch:
    → All TLB entries invalidated → every next access is a TLB miss
    → Page table walks for ALL accessed pages in process B
    → 100s–1000s of extra cache/memory accesses per context switch
    → Context switch overhead: 1–10 µs becomes 10–100 µs
    → On 10,000 ctx/sec: wastes 100–1000 µs per second = 1–10% CPU

With ASIDs:
  Each process gets a unique ASID (8-bit: 256 IDs, 16-bit: 65536 IDs)
  TLB entries are tagged with the ASID of the process that owns them.
  
  On context switch (A → B):
    Load B's ASID+TTBR0 into TTBR0_EL1
    TLB still contains A's entries BUT they are tagged with A's ASID
    TLB also contains B's entries tagged with B's ASID (from previous run)
    New accesses: TLB lookup uses B's current ASID → ONLY B's entries match!
    A's entries are invisible (wrong ASID tag) but NOT removed
    
    No TLB flush needed! TLB entries from previous processes coexist.
    On B's reuse: if B ran recently, its TLB entries may still be valid → instant hit!
```

---

## 2. ARM64 ASID Encoding

```
ASID is stored in TTBR0_EL1 and TTBR1_EL1:

TTBR0_EL1[63:48] = ASID (16-bit, or lower 8 bits used if TCR_EL1.AS=0)
TTBR0_EL1[47:1]  = BADDR (base address of L0 page table, TTBR-aligned)
TTBR0_EL1[0]     = CnP (Common not Private, ARMv8.2)

TCR_EL1.AS bit[36]:
  AS=0: 8-bit ASID (TTBR[55:48] used as ASID, [63:56] ignored)
        256 possible ASIDs
  AS=1: 16-bit ASID (TTBR[63:48] all used)
        65536 possible ASIDs

A1 bit in TCR_EL1[22]:
  A1=0: ASID taken from TTBR0_EL1[63:48] (default, Linux uses this)
  A1=1: ASID taken from TTBR1_EL1[63:48]
  Linux default: A1=0 (ASID from TTBR0)

ASID 0 is special:
  Used for "global" mappings (nG=0 in PTE)
  Global entries match ANY ASID — they are shared across all processes
  Kernel mappings via TTBR1_EL1 are global (nG=0 for most kernel pages)
  User pages typically: nG=1 (ASID-tagged — only match correct ASID)
```

---

## 3. ASID Allocation in Linux Kernel

```
ASID management: arch/arm64/mm/context.c

Data structures:
  atomic64_t asid_generation;     // Current generation number (upper bits)
  unsigned long *asid_map;         // Bitmap: which ASIDs in current generation are used
  DEFINE_PER_CPU(atomic64_t, active_asids);  // Per-CPU currently active ASID

ASID format (Linux internal):
  asid = generation | asid_value
  Bits [63:ASID_BITS] = generation counter
  Bits [ASID_BITS-1:0] = actual ASID value

Allocation algorithm:
  new_context(mm, cpu):
    asid = atomic64_read(&mm->context.id)
    if (asid_generation(asid) == current_generation):
        // ASID still valid for current generation → reuse
        reserve_asid_and_return(asid)
    
    // ASID is from old generation → need new ASID
    asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx)
    if (asid == NUM_USER_ASIDS):
        // ALL ASIDs used in this generation → ROLLOVER!
        flush_context()  // Invalidate ALL TLBs system-wide
        generation++
        memset(asid_map, 0)  // All ASIDs free again in new generation
        asid = 1             // ASID 0 reserved for global entries
    
    set_bit(asid, asid_map)
    // Store: generation | asid → mm->context.id
    return generation | asid
```

---

## 4. ASID Rollover: The Full TLB Flush Event

```
ASID rollover occurs when all ASID values in the current generation are exhausted.
For 8-bit ASID: after 255 unique processes have active ASIDs (ASID 0 reserved)
For 16-bit ASID: after 65535 unique processes

Rollover sequence:
  1. All ASIDs used → allocation fails → increment generation counter
  2. TLBI VMALLE1IS: broadcast TLB invalidation to all CPUs in ISH domain
     ALL TLB entries (all ASIDs, all processes) are invalidated globally
  3. Reset asid_map bitmap: all ASIDs free
  4. Redistribute ASIDs: currently active processes get new ASIDs from generation N+1
  5. Continue scheduling

Cost:
  TLB flush flushes EVERYTHING on ALL cores
  All active threads on all CPUs will suffer TLB misses until refill
  On a 16-core system with 65535 ASIDs: rollover rarely happens
  On 8-bit ASID (256 IDs): rollover more frequent on systems with many processes
  
  Linux uses 16-bit ASIDs on ARM64 by default (CONFIG_ARM64_VA_BITS_48):
    TCR_EL1.AS=1 (if hardware supports FEAT_ASID16, which is common)
    Rollover happens only after 65535 processes/contexts are active → very rare

CnP (Common not Private, ARMv8.2):
  TTBR.CnP=1: all CPUs sharing same translation regime share one TLB set
  Reduces TLB entries needed when SMP cores share same memory map
  Kernel mapping (TTBR1) can use CnP=1: same kernel for all cores
  Process TTBR0: usually CnP=0 (each process has private mapping)
```

---

## 5. Context Switch: Atomic ASID + TTBR Update

```
Context switch with ASID (arch/arm64/kernel/entry.S):

  switch_mm(old_mm, new_mm, tsk):
    1. Get next_asid for new_mm (may allocate new ASID)
    2. Build new TTBR0 value:
       next_ttbr0 = (asid << 48) | page_table_phys_addr
       
    3. Atomic update:
       // CRITICAL: ASID and BADDR must be written atomically
       // If ASID written first: wrong ASID with old table → wrong translations
       // If BADDR written first: new table with old ASID → wrong ASID match
       // Solution: write both together as single 64-bit MSR
       
       msr TTBR0_EL1, next_ttbr0   // 64-bit write: ASID + base addr atomically
       isb                          // Ensure new TTBR is visible to subsequent accesses
    
    4. TLB invalidation (if needed):
       If ASID is newly allocated from current generation:
         No TLB flush needed (new ASID has no stale entries)
       If ASID was reserved but previously used (highly unlikely):
         TLBI ASIDE1IS, Xt  // Flush entries for this specific ASID

Why single 64-bit write is atomic:
  ARM64 store of a 64-bit aligned value to TTBR0_EL1 is guaranteed atomic
  The ASID and base address change simultaneously → no intermediate state
  Hardware sees either old (asid+table) or new (asid+table), never mixed
```

---

## 6. nG Bit and ASID Tagging

```
nG (not Global) bit in PTE[11]:
  nG=0: Global entry (no ASID tag) → matches all ASIDs
        Used for kernel mappings (TTBR1) that should be visible to ALL processes
        
  nG=1: Non-global entry (ASID-tagged) → only matches entries with same ASID
        Used for user-space page mappings (TTBR0)
        When stored in TLB: ASID from current TTBR is stored with the entry
        On lookup: TLB only returns this entry if current ASID matches

Linux behavior:
  All user pages (TTBR0 mappings): nG=1
    → ASID-tagged → one process's pages invisible to other processes' TLB lookups
    
  Kernel pages (TTBR1 mappings): nG=0
    → Global → all processes share the same kernel TLB entries
    → Kernel text/data TLB entries don't need per-process duplication
    
  Exception: KPTI (Kernel Page Table Isolation):
    In KPTI, user-mode trampoline PTE has nG=0 (global in user page tables)
    Kernel full mappings are nG=0 but only present in kernel page tables
    When switching to kernel mode: TTBR1 still points to full kernel table
    User-mode page table has only the minimal trampoline mapping
```

---

## 7. Interview Questions & Answers

**Q1: What happens when ARM64's ASID space is exhausted (rollover)?**

When all ASID values in the current generation are exhausted, Linux performs an ASID rollover:
1. The generation counter is incremented (stored in the high bits of the ASID values in `mm->context.id`).
2. `TLBI VMALLE1IS` is executed — this invalidates ALL TLB entries on ALL CPUs in the inner shareable domain (entire SoC).
3. The ASID bitmap is cleared — all ASID values are now free for redistribution.
4. The next context switch will allocate ASID values starting from ASID 1 again (ASID 0 is reserved for global entries).
5. Processes that were previously active but haven't been scheduled since the rollover will get new ASID values on their next context switch.

The rollover is expensive (full TLB flush across all CPUs) but rare. With 16-bit ASIDs (65535 unique values), you'd need 65535 active contexts before a rollover — unusual on most systems. The generation number also prevents old ASIDs (from before the flush) from being matched against post-rollover translations.

---

## 8. Quick Reference

| TCR_EL1.AS | ASID width | Max processes w/o flush |
|---|---|---|
| 0 | 8-bit | 255 (ASID 0 reserved for global) |
| 1 | 16-bit | 65535 |

| nG bit | Type | ASID match? | Used for |
|---|---|---|---|
| 0 | Global | Any ASID | Kernel mappings (TTBR1) |
| 1 | Non-global | Must match | User process mappings (TTBR0) |

| Event | TLB invalidation needed? |
|---|---|
| Context switch (different ASID) | No |
| Context switch (same ASID, different mm) | Yes (but shouldn't happen with proper ASID allocation) |
| ASID rollover | Yes (TLBI VMALLE1IS — full flush) |
| munmap / mprotect | Yes (TLBI VAE1IS for affected VAs) |
