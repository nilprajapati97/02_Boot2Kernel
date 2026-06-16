# nG Bit: Non-Global Entries and ASID Tagging

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

The **nG (non-Global) bit** in an ARM64 page/block descriptor determines whether a TLB entry is tagged with an ASID (Address Space Identifier):

- **nG=0 (Global)**: TLB entry is NOT tagged with an ASID. Valid for all ASIDs. Kernel mappings use this — they must be accessible regardless of which process is running.
- **nG=1 (Non-Global)**: TLB entry IS tagged with the ASID at the time of the TLB fill. Only valid for the current process's ASID.

Without nG/ASID, every context switch would require flushing the entire TLB (to prevent one process from accessing another's mappings). ASID-tagged TLB entries allow TLB entries from different processes to coexist — context switch only needs to change the ASID, not flush the TLB.

---

## 2. ASID in TTBR0_EL1

The current process ASID is stored in `TTBR0_EL1[63:48]`:

```
TTBR0_EL1:
  Bits[63:48] = ASID  (8 or 16 bits, depending on TCR_EL1.AS)
  Bits[47:1]  = BADDR (Page table base address >> 1, in some configurations)
  Bit[0]      = CnP   (Common not Private, ARMv8.2)

Actually for most configurations:
  Bits[63:48] = ASID
  Bits[47:0]  = Page table base address (bits[47:0], must be page-aligned)
  → Effective base: TTBR0_EL1[47:12] << 12 (lower 12 bits must be 0)

When TCR_EL1.A1=0 (default): ASID from TTBR0_EL1[63:48]
When TCR_EL1.A1=1:           ASID from TTBR1_EL1[63:48]
```

---

## 3. TLB Lookup with nG and ASID

ARM64 TLB lookup key consists of:

```
TLB Key (for nG=1 entries):
  { ASID[15:0] , VA[47:12] , Inner/Outer Shareable domain, VMID (EL2) }

TLB Key (for nG=0 / Global entries):
  { VA[47:12] , Inner/Outer Shareable domain, VMID (EL2) }
  (ASID not included — matches any ASID)
```

### Lookup Process

```
CPU issues VA access:
  1. Check nG bit expectation:
     - If VA[63:48] = 0x0000 (user) → potential nG=1 entry
     - If VA[63:48] = 0xFFFF (kernel) → expect nG=0 entry
     
  2. TLB search:
     - Match nG=0 entries by VA only (any ASID)
     - Match nG=1 entries by (ASID + VA) pair
     
  3. If match found → attributes from TLB entry
  4. If no match → PTW (Page Table Walk) → fills TLB with new entry
     (TLB entry inherits nG bit from descriptor, captures current ASID)
```

---

## 4. Context Switch and ASID Change

### Without ASID (hypothetical)

```
Process A maps VA=0x1000 → PA=0x10000 (user data)
Process B maps VA=0x1000 → PA=0x20000 (different user data)

Without ASID: 
  Context switch A→B: TLB still has "VA=0x1000 → PA=0x10000" from A
  Process B accesses VA=0x1000 → hits A's TLB entry → reads A's data
  → SECURITY DISASTER (cross-process data leakage)
  → Must flush entire TLB on every context switch

With TLB flush on every context switch:
  TLB effective size: ~0 (flushed on every switch)
  Overhead: context switch must invalidate all ~1024-4096 TLB entries
  → Massive performance cost
```

### With ASID

```
Process A: ASID = 42
Process B: ASID = 17

TLB contents after A runs:
  {ASID=42, VA=0x1000} → PA=0x10000 (nG=1 entry)
  {global, VA=0xFFFF...} → PA=... (nG=0 kernel entries)

Context switch A→B:
  Write TTBR0_EL1 = (17 << 48) | process_B_pgd_pa
  
TLB lookup for B accessing VA=0x1000:
  Check TLB for {ASID=17, VA=0x1000} → no match (A's entry has ASID=42)
  → PTW walk using B's page tables
  → Find PA=0x20000 for B's VA=0x1000
  → TLB fill: {ASID=17, VA=0x1000} → PA=0x20000

Both entries coexist in TLB:
  {ASID=42, VA=0x1000} → PA=0x10000  (A's entry, not used while B runs)
  {ASID=17, VA=0x1000} → PA=0x20000  (B's entry)
→ No cross-process leakage, no TLB flush needed on switch
```

---

## 5. ASID Allocation in Linux

```c
// arch/arm64/mm/context.c

// ASID bitmap for 8-bit ASIDs (256 total):
static unsigned long *asid_map;         // Bitmap of allocated ASIDs
static atomic64_t asid_generation;     // Current generation counter
static DEFINE_PER_CPU(atomic64_t, active_asids);  // Per-CPU current ASID

// Linux uses a combined 64-bit value:
// asid_value = (generation << ASID_BITS) | asid_number
// Upper bits = "generation" epoch
// Lower bits = actual ASID value (0-255 for 8-bit, 0-65535 for 16-bit)

// ASID 0 is reserved (for kernel/init thread, which has no user ASID)
// Usable ASIDs: 1 to (2^ASID_BITS - 1)
```

### ASID Assignment

```c
static u64 new_context(struct mm_struct *mm)
{
    u64 asid = atomic64_read(&mm->context.id);
    u64 generation = atomic64_read(&asid_generation);
    
    if (asid != 0) {
        u64 newasid = generation | (asid & ~ASID_MASK);
        // Check if same ASID still in current generation:
        if (check_update_reserved_asid(asid, newasid))
            return newasid;
        // Reuse the asid if safe:
        if (!test_and_set_bit(asid & ~ASID_MASK, asid_map))
            return newasid;
    }
    
    // Allocate new ASID:
    asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
    if (asid != NUM_USER_ASIDS)
        goto set_asid;
    
    // No free ASIDs → roll over generation:
    generation = atomic64_add_return_relaxed(ASID_FIRST_VERSION, &asid_generation);
    flush_context();  // TLB flush all ASID-tagged entries (global rollover!)
    bitmap_zero(asid_map, NUM_USER_ASIDS);
    
    asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);
    
set_asid:
    __set_bit(asid, asid_map);
    cur_idx = asid;
    return asid | generation;
}
```

### ASID Rollover (Generation Change)

```
When all ASIDs (256 or 65536) are exhausted:
  1. Increment "generation" counter
  2. TLBI VMALLE1IS — flush ALL ASID-tagged TLB entries system-wide
  3. Reset bitmap → all ASIDs "free" in new generation
  4. Re-allocate ASIDs starting from 1

Processes that haven't run yet in the new generation:
  → Will get a new ASID when they next switch in
  → mm->context.id has old generation → detected on context switch
  → New ASID allocated; TTBR0 updated

Performance: ASID rollover is expensive (TLB flush + bitmap reset)
  → With 16-bit ASIDs (65536 entries): rare event (need 65535 unique processes)
  → With 8-bit ASIDs (256 entries): may happen frequently on busy servers
  → Linux prefers 16-bit if ID_AA64MMFR0_EL1.ASIDBits=1
```

---

## 6. nG and Kernel Mappings

```
All kernel mappings (TTBR1 region, VA[63:48]=0xFFFF) use nG=0 (global):

Why global for kernel?
  1. Kernel code/data is shared by all processes
  2. nG=0 entries are never invalidated by ASID change
  3. Kernel TLB entries survive context switches → TLB warmth maintained
  4. No ASID needed — kernel has no per-process address space

Example: Kernel text at 0xFFFF000010080000
  PTE.nG = 0 (global)
  TLB entry: {VA=0xFFFF000010080000} → PA (no ASID in key)
  → Valid for ALL processes (context switch doesn't invalidate this)

Exception: KPTI (Kernel Page Table Isolation, Meltdown mitigation)
  KPTI creates a "user stub" kernel page table (trampoline pages)
  These use nG=1 (non-global) for isolation from user ASID
  See: arch/arm64/kernel/entry.S __swpan_entry_el1 (SWAPPER_DIR_SIZE)
```

---

## 7. TLB Flush Operations and nG

ARM64 TLBI instructions differentiate between global and non-global entries:

```assembly
// Flush ALL TLB entries (global + non-global):
TLBI VMALLE1IS      // Inner Shareable, EL1, all ASIDs, all VAs
TLBI VMALLE1        // Local only (current CPU)

// Flush specific VA for specific ASID (non-global only):
TLBI VAE1IS, Xt    // Xt[63:48]=ASID, Xt[43:0]=VA>>12; IS=Inner Shareable
TLBI VAE1, Xt      // Local only

// Flush specific VA, all ASIDs (global entries for that VA):
TLBI VAAE1IS, Xt   // VA[43:0]=Xt[43:0]>>0; ALL ASIDs; IS

// Flush by ASID (all entries for a specific ASID):
TLBI ASIDE1IS, Xt  // Xt[63:48]=ASID; flush all TLB entries with that ASID
TLBI ASIDE1, Xt    // Local only

// Linux usage:
// munmap (unmap specific VA, user process):
//   flush_tlb_range → TLBI VAE1IS (with ASID+VA)
// Process exit (free all ASIDs):
//   flush_tlb_mm → TLBI ASIDE1IS (with process ASID)
// Kernel page unmap:
//   flush_tlb_kernel_range → TLBI VAAE1IS (no ASID)
```

---

## 8. nG and CnP (ARMv8.2 Common-not-Private)

```
CnP (bit[0] of TTBR0/TTBR1, ARMv8.2):
  CnP=1: All CPUs in the Inner Shareable domain use the same translation tables
  → TLB can be shared across CPUs for these table entries (hardware hint)
  → Reduces TLB invalidation broadcast overhead on multi-core

CnP interaction with nG:
  With CnP=1: TTBR0 value (including ASID) is broadcast to all CPUs
  All CPUs that share the same CnP domain must use the same ASID for nG entries
  → This is guaranteed by Linux: all CPUs run same mm → same ASID

Linux enables CnP:
// arch/arm64/mm/proc.S
#ifdef CONFIG_ARM64_CNP
    alternative_if ARM64_HAS_CNP
    orr     ttbr1, ttbr1, #TTBR_CNP_BIT
    alternative_else_nop_endif
#endif
```

---

## 9. Interview Questions & Answers

**Q1: Why do all user space mappings use nG=1 but kernel mappings use nG=0?**

User space mappings are private to each process. If nG=0, TLB entries for user pages would be global — any process could use them, leading to cross-process memory access (security violation). With nG=1, each user page TLB entry is tagged with the process's ASID. On context switch, changing TTBR0_EL1 (and the ASID in its upper bits) immediately invalidates all TLB lookups for the old process's user pages without needing a TLB flush.

Kernel mappings use nG=0 because the kernel is shared: all processes share the same kernel address space. Making kernel TLB entries global means they survive context switches, keeping the kernel TLB warm regardless of process switches. An ASID change only invalidates nG=1 (user) entries, not nG=0 (kernel) entries — this is the performance win of the nG/ASID design.

**Q2: What happens when all ASIDs are exhausted in Linux?**

Linux performs an ASID "rollover" (generation change). It increments the global generation counter, executes `TLBI VMALLE1IS` to flush all ASID-tagged TLB entries across all CPUs, resets the ASID allocation bitmap, and starts re-allocating from ASID 1. Processes that haven't run in the new generation are detected on context switch by comparing `mm->context.id`'s generation field with `asid_generation`. They receive a new ASID assignment. With 8-bit ASIDs (256 ASIDs), rollover can happen after 255 unique processes run — common on servers. With 16-bit ASIDs (65535 usable), rollover is much rarer. The rollover itself is expensive due to the global TLB flush.

**Q3: Can a global TLB entry (nG=0) and a non-global entry (nG=1) for the same VA coexist?**

Technically they have different TLB lookup keys: the global entry matches by VA alone, the non-global entry by (ASID, VA). However, ARM64 architecture prohibits having both types simultaneously for the same VA (it would be a TLB conflict = UNPREDICTABLE behavior). Linux prevents this by ensuring kernel VAs (nG=0, TTBR1 region) and user VAs (nG=1, TTBR0 region) are disjoint VA ranges. The VA split (user space = VA[63:48]=0x0000, kernel = VA[63:48]=0xFFFF) ensures no VA conflict between global and non-global entries.

---

## 10. Quick Reference

| Aspect | nG=0 (Global) | nG=1 (Non-Global) |
|---|---|---|
| TLB key | VA only (no ASID) | (ASID, VA) |
| Survives context switch | Yes | Only if ASID unchanged |
| Used for | Kernel mappings | User mappings |
| TTBR region | TTBR1 (typically) | TTBR0 |
| TLB flush to invalidate | `TLBI VAE1IS` (no ASID) | `TLBI VAE1IS` with ASID |
| Performance | Warm across context switches | Per-ASID TLB efficiency |
| Security | Shared across processes | Private per-ASID |

| TLBI Instruction | Target | ASID-aware |
|---|---|---|
| `VMALLE1IS` | All EL1 entries, all ASIDs | No (flushes everything) |
| `VAE1IS Xt` | Specific VA, specific ASID | Yes (ASID in Xt[63:48]) |
| `VAAE1IS Xt` | Specific VA, all ASIDs | No (ignores ASID) |
| `ASIDE1IS Xt` | All VAs for specific ASID | Yes |
