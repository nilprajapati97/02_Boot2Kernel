# ARM32 TLB Management and ASID Deep Dive
## Document 4: Translation Lookaside Buffer — Architecture, Operations, SMP

**Author:** Senior Kernel Engineer  
**Target:** ARMv7-A (ARM32), Linux Kernel v5.x/v6.x  
**Scope:** TLB micro-architecture, ASID lifecycle, SMP shootdown, debugging  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview), Document 03 (Linux MM Internals)

---

## Table of Contents
1. [TLB Micro-Architecture](#1-tlb-micro-architecture)
2. [TLB Entry Lifecycle](#2-tlb-entry-lifecycle)
3. [ASID Architecture and Lifecycle](#3-asid-architecture-and-lifecycle)
4. [TLB Maintenance Operations — Complete Reference](#4-tlb-maintenance-operations--complete-reference)
5. [SMP TLB Shootdown](#5-smp-tlb-shootdown)
6. [TLB Lockdown](#6-tlb-lockdown)
7. [Linux Kernel TLB API](#7-linux-kernel-tlb-api)
8. [TLB and Cache Interaction](#8-tlb-and-cache-interaction)
9. [Performance Analysis and Profiling](#9-performance-analysis-and-profiling)
10. [Cortex-A Series TLB Specifics](#10-cortex-a-series-tlb-specifics)
11. [Common TLB Bugs and Debugging](#11-common-tlb-bugs-and-debugging)

---

## 1. TLB Micro-Architecture

### 1.1 Typical ARM Cortex-A TLB Organization

```
ARM Cortex-A9 (representative modern ARMv7-A):

┌──────────────────────────────────────────────────────────────┐
│                   Instruction TLB (ITLB)                     │
│   Fully Associative | 32 entries | ASID-tagged              │
│   Supports: 4KB, 64KB, 1MB, 16MB page sizes                 │
│   Replacement: Pseudo-Random (PLRU variant)                  │
└──────────────────────────────────────────────────────────────┘
         ↑ Instruction fetch misses fill from L2 TLB

┌──────────────────────────────────────────────────────────────┐
│                     Data TLB (DTLB)                          │
│   Fully Associative | 32 entries | ASID-tagged              │
│   Supports: 4KB, 64KB, 1MB, 16MB page sizes                 │
│   Replacement: Pseudo-Random (PLRU variant)                  │
└──────────────────────────────────────────────────────────────┘
         ↑ Data access misses fill from L2 TLB

┌──────────────────────────────────────────────────────────────┐
│                  Unified L2 TLB (Main TLB)                   │
│   4-Way Set-Associative | 128 entries                        │
│   Shared between ITLB and DTLB                               │
│   Filled by hardware page table walker                        │
└──────────────────────────────────────────────────────────────┘
         ↑ L2 TLB miss triggers hardware page table walk

┌──────────────────────────────────────────────────────────────┐
│              Hardware Page Table Walker (PTW)                 │
│   Reads TTBR0/TTBR1 → L1 descriptor → L2 descriptor         │
│   Access L1 cache (if PT memory is cached Normal)            │
│   Generates Translation Fault on invalid entry               │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 TLB Entry Fields

```
Conceptual TLB Entry Structure:

 63        56 55    32 31        12 11  9   8   7   6   5   4   3   2   1   0
┌───────────┬────────┬────────────┬─────┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│  ASID[7:0]│ VMID   │ VA Tag     │ SZ  │nG │ S │APX│AP │XN │Domain│ C │ B │V│
└───────────┴────────┴────────────┴─────┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
                                                           ↑ Physical Address Tag

Fields:
  ASID[7:0]   : Address Space ID (from CONTEXTIDR[7:0])
  VMID        : Virtual Machine ID (Stage-2, only in Hyp mode, ARM virt extension)
  VA Tag      : Virtual Address tag (bits [31:12] or higher for large pages)
  SZ          : Page Size (4KB/64KB/1MB/16MB)
  nG          : Not-Global (0=global=kernel, 1=process-specific)
  S           : Shareable
  APX, AP     : Access Permissions
  XN          : Execute Never
  Domain      : Domain[3:0]
  C, B        : Cache/Buffer attributes
  V           : Valid bit
  PA          : Physical Address base
```

### 1.3 TLB Hit Path (Timing Critical)

```
VA presented to TLB:

Cycle 0: VA[31:12] tag compared against all TLB entries simultaneously
         (Fully-associative → all entries checked in parallel via CAM)

Cycle 0 (parallel): Check ASID match:
         - If nG=1: ASID must match CONTEXTIDR[7:0]
         - If nG=0: Global entry, ASID ignored

Cycle 1: Hit determined, permission check:
         - AP/APX vs access type (read/write, privileged/user)
         - XN vs instruction fetch
         - Domain check vs DACR

Cycle 1: PA formed:
         - PA[31:12] from TLB entry
         - PA[11:0] from VA[11:0] (for 4KB pages)
         - PA[19:0] from VA[19:0] (for 1MB sections)
```

### 1.4 TLB Miss and Page Table Walk

```
On TLB Miss (L1 + L2 both miss):

Hardware PTW reads TTBR0 or TTBR1:
  Decision: VA[31:32-N] == all 1's → use TTBR1 (kernel)
            else → use TTBR0 (user)

Step 1: L1 Walk
  PA_L1 = TTBR[31:14] : VA[31:20] : 0b00   (word-aligned)
  descriptor_L1 = MEM[PA_L1]
  
  If descriptor_L1[1:0] == 0b10 → Section
     PA = descriptor_L1[31:20] : VA[19:0]
     Load into TLB, done.
  
  If descriptor_L1[1:0] == 0b01 → Page Table
     PA_L2_base = descriptor_L1[31:10] : 0b00_0000_0000

Step 2: L2 Walk
  PA_L2 = PA_L2_base : VA[19:12] : 0b00     (word-aligned)
  descriptor_L2 = MEM[PA_L2]
  
  If descriptor_L2[1:0] == 0b10 → Small Page (4KB)
     PA = descriptor_L2[31:12] : VA[11:0]
     Load into TLB, done.
  
  If descriptor_L2[1:0] == 0b01 → Large Page (64KB)
     PA = descriptor_L2[31:16] : VA[15:0]
     16 identical TLB entries loaded, done.
  
  If descriptor_L2[1:0] == 0b00 → Translation Fault
     Generate Data/Prefetch Abort
```

---

## 2. TLB Entry Lifecycle

### 2.1 Entry Creation

```
Trigger: Hardware page table walk completes successfully
Action:  New entry installed in L2 TLB (evicts LRU entry if full)
         Entry promoted to L1 ITLB/DTLB on access
         
Note: Software cannot explicitly "insert" TLB entries on standard ARMv7-A
      (unlike MIPS). All fills are hardware-driven by the PTW.
      Exception: TLB lockdown (implementation-defined, see Section 6)
```

### 2.2 Entry Invalidation (Explicit)

Software must explicitly invalidate TLB entries when:
- A page table entry is changed (PA, permissions, attributes)
- A mapping is removed (munmap, kfree_pages)
- A process exits (PGD freed)
- ASID generation rolls over

### 2.3 Entry Replacement (Implicit)

```
L1 TLB (32 entries, fully associative):
  - Pseudo-random replacement (implementation-defined)
  - Some cores: Round-robin
  - NO software control of replacement policy on standard ARMv7-A

L2 TLB (128 entries, 4-way):
  - LRU within each set
  - Set index derived from VA bits (implementation-defined)
```

---

## 3. ASID Architecture and Lifecycle

### 3.1 Purpose of ASID

Without ASID, every context switch requires a full TLB flush:
```
Context switch WITHOUT ASID:
  flush_tlb_all()     ← flush ALL user TLB entries
  write TTBR0         ← point to new process PGD
  
  Next 100-1000 memory accesses: ALL TLB misses
  Cost: ~100-200 cycles each miss × 1000 misses = 100,000-200,000 cycles wasted
```

With ASID:
```
Context switch WITH ASID:
  write CONTEXTIDR    ← new ASID (no TLB flush needed for global entries)
  write TTBR0         ← new PGD
  
  Old process TLB entries tagged with old ASID → ignored (not flushed)
  New process entries with new ASID → may already be cached from last run
  Cost: Only misses for pages not in TLB already
```

### 3.2 ASID Registers

```assembly
/* CONTEXTIDR (Context ID Register, CP15 c13 c0 1) */
/* [31:8] PROCID — software process ID (informational, no HW effect) */
/* [7:0]  ASID   — hardware ASID used for TLB tagging */

/* Read ASID */
MRC p15, 0, r0, c13, c0, 1    @ r0 = CONTEXTIDR
AND r0, r0, #0xFF              @ r0 = ASID[7:0]

/* Write ASID + PROCID */
ORR r0, r0, r1, LSL #8         @ r0 = PROCID<<8 | ASID
MCR p15, 0, r0, c13, c0, 1    @ CONTEXTIDR = r0
ISB                             @ ensure ASID active before next translation
```

### 3.3 ASID Namespace

```
ARM32: 8-bit ASID → 256 unique values (0–255)
ARM64: 16-bit ASID option → 65536 unique values

ASID 0: Reserved for kernel (global mappings with nG=0 don't need ASID)

Linux allocation:
  asid_bits = 8 (ARM32)
  Usable ASIDs: 1–255 (255 user processes without flush)
  
  Process count >> 255 on modern systems → need ASID generation/rotation
```

### 3.4 ASID Generation Counter (Linux Implementation)

```c
/* arch/arm/mm/context.c */

/*
 * Global generation counter. Upper bits = generation, lower 8 bits = reserved.
 * Each mm stores: mm->context.id = generation | ASID
 *
 * Valid if: (mm->context.id >> ASID_BITS) == (asid_generation >> ASID_BITS)
 */
static atomic64_t asid_generation;

/* Per-CPU: active ASID on this CPU */
static DEFINE_PER_CPU(atomic64_t, active_asids);

/* Reserved ASIDs: one per CPU (for tlb_flush_pending) */
static DEFINE_PER_CPU(u64, reserved_asids);

/* Bitmap of allocated ASIDs in current generation */
static unsigned long *asid_map;

static u64 new_context(struct mm_struct *mm, unsigned int cpu)
{
    static u32 cur_idx = 1;   /* start allocating from ASID 1 */
    u64 asid = atomic64_read(&mm->context.id);
    u64 generation = atomic64_read(&asid_generation);

    if (asid != 0) {
        u64 newasid = generation | (asid & ~ASID_MASK);
        /* Re-use same ASID in new generation if not taken */
        if (!__test_and_set_bit(asid & ASID_MASK, asid_map))
            return newasid;
    }

    /* Find a free ASID slot */
    asid = find_next_zero_bit(asid_map, NUM_ASIDS, cur_idx);
    if (asid == NUM_ASIDS) {
        /* ASID space exhausted — new generation */
        generation = atomic64_add_return(NUM_ASIDS, &asid_generation);
        flush_tlb_all_on_all_cpus();     /* broadcast TLB flush */
        bitmap_clear(asid_map, 0, NUM_ASIDS);
        asid = 1;   /* Start fresh */
    }

    __set_bit(asid, asid_map);
    cur_idx = asid;
    return generation | asid;
}
```

### 3.5 Generation Rollover — Full TLB Flush

```
Scenario: 256 processes have been given ASIDs 1–255.
          Process 257 needs an ASID.

Action:
  1. Increment generation counter (upper bits of asid_generation)
  2. Send IPI to all CPUs: "flush your entire TLB"
  3. Clear asid_map bitmap
  4. Reallocate ASID 1 to process 257
  5. All other processes: their mm->context.id has stale generation
     → Next time they run: check_and_switch_context() detects mismatch
     → Slow path: allocate fresh ASID in new generation

Cost: O(nr_cpus) IPI + full TLB flush on every CPU
Frequency: Every 255 new processes created (on ARM32)
```

### 3.6 SMP ASID Safety

```
Problem: CPU0 assigns ASID N to process A.
         CPU1 still running process A with ASID N in CONTEXTIDR.
         CPU0 now assigns ASID N to process B (new generation).
         CPU1 might serve TLB hits for process A using ASID N → WRONG!

Solution:
  per_cpu(reserved_asids)[cpu] = old ASID
  During generation rollover:
    - Check if any CPU has reserved the ASID
    - Reserve new ASID for each active CPU
    - Only after all CPUs have flushed their TLB is ASID safe to reuse
  Uses IPI + synchronize_rcu() barrier
```

---

## 4. TLB Maintenance Operations — Complete Reference

### 4.1 CP15 c8 Register Map

```
MCR p15, 0, Rd, c8, CRm, op2

CRm = c5 → Instruction TLB operations
CRm = c6 → Data TLB operations
CRm = c7 → Unified TLB operations

op2 = 0 → Invalidate entire TLB (ignore Rd)
op2 = 1 → Invalidate TLB entry by MVA (Rd = MVA)
op2 = 2 → Invalidate TLB entries by ASID (Rd[7:0] = ASID)
op2 = 3 → Invalidate TLB entry by MVA, All ASIDs (Rd = MVA)
```

### 4.2 Complete TLB Invalidation Operations

```assembly
/*============================================================
 * UNIFIED TLB (Data + Instruction)
 *============================================================*/

/* Invalidate entire Unified TLB (Inner Shareable) */
/* Use on context switch or after mapping large ranges */
MCR p15, 0, r0, c8, c7, 0  @ TLBIALL

/* Invalidate Unified TLB by MVA (current ASID implied) */
/* Use when: single page mapping changed */
/* r0 = MVA (must be page-aligned, bits [11:0] ignored by HW) */
MCR p15, 0, r0, c8, c7, 1  @ TLBIMVA

/* Invalidate Unified TLB by ASID */
/* Use when: process exits, ASID recycled */
/* r0[7:0] = ASID to flush */
MCR p15, 0, r0, c8, c7, 2  @ TLBIASID

/* Invalidate Unified TLB by MVA, all ASIDs */
/* Use when: kernel mapping changed (visible to all processes) */
MCR p15, 0, r0, c8, c7, 3  @ TLBIMVAA

/*============================================================
 * INSTRUCTION TLB ONLY
 *============================================================*/
MCR p15, 0, r0, c8, c5, 0  @ ITLBIALL  — invalidate all ITLB
MCR p15, 0, r0, c8, c5, 1  @ ITLBIMVA  — by MVA
MCR p15, 0, r0, c8, c5, 2  @ ITLBIASID — by ASID

/*============================================================
 * DATA TLB ONLY
 *============================================================*/
MCR p15, 0, r0, c8, c6, 0  @ DTLBIALL  — invalidate all DTLB
MCR p15, 0, r0, c8, c6, 1  @ DTLBIMVA  — by MVA
MCR p15, 0, r0, c8, c6, 2  @ DTLBIASID — by ASID
```

### 4.3 DSB Requirement Around TLB Operations

```assembly
/*
 * CRITICAL: ARM Architecture Rule
 *
 * Before TLB invalidate: ensure page table write is visible to PTW
 * After TLB invalidate: ensure TLB is clean before new accesses
 *
 * Required sequence when changing a PTE:
 */

/* 1. Write new PTE value to page table */
STR     r2, [r1]           @ page_table[index] = new_pte

/* 2. Data Synchronization Barrier — ensure PTW sees new PTE */
DSB                         @ wait for all previous mem ops to complete

/* 3. TLB invalidate — remove stale entry */
MCR     p15, 0, r0, c8, c7, 1   @ TLBIMVA: invalidate VA in all TLBs

/* 4. Another DSB — ensure TLB invalidate completes */
DSB                         @ ensure TLB clean before continuing

/* 5. ISB — if the next instruction might use the new mapping */
ISB                         @ flush pipeline
```

> **Interview Q:** "Why do you need DSB before TLB invalidate, not just after?"  
> **A:** Because the hardware page table walker runs asynchronously from the CPU pipeline. Without DSB, the PTW might still use the old PTE to fill the TLB *after* you've done the TLBI, leaving a stale entry again. DSB before TLBI ensures the new PTE is in memory first.

### 4.4 MVA Format for TLBIMVA

```
TLBIMVA register format:

[31:12] Modified Virtual Address (page aligned)
[11:8]  Reserved (SBZ)
[7:0]   ASID (optional — only used by TLBIMVA if ASID-aware variant)

Example: Flush VA 0xBEEF1000 for ASID 0x42:
  LDR r0, =0xBEEF1042    @ bits[31:12]=0xBEEF1, bits[7:0]=0x42
  MCR p15, 0, r0, c8, c7, 3   @ TLBIMVAA (all ASIDs at that VA)

OR: Use current ASID (embedded in CONTEXTIDR):
  LDR r0, =0xBEEF1000
  MCR p15, 0, r0, c8, c7, 1   @ TLBIMVA (current ASID only)
```

---

## 5. SMP TLB Shootdown

### 5.1 Why SMP Needs Shootdown

```
CPU0 maps page X at VA 0x1000 for process P (ASID=5).
Process P runs on CPU0 and CPU1 simultaneously (multi-thread).
CPU0 unmaps page X (munmap).
CPU0 updates PTE: present=0, flushes its local TLB.
CPU1 still has the old TLB entry: VA=0x1000, ASID=5 → PA=old_page

CPU1 accesses VA 0x1000 → TLB HIT (stale) → accesses freed physical page!
→ Memory corruption / security vulnerability
```

### 5.2 TLB Shootdown Mechanism

```
ARM32 has NO hardware-assisted TLB shootdown.
(Unlike x86 with INVPCID or ARM64 with TLBI IS broadcast)

ARM32 software TLB shootdown:

1. CPU0 updates page table (clears PTE)
2. CPU0 calls flush_tlb_page() or flush_tlb_mm()
3. flush_tlb_page() → on_each_cpu_mask() → IPI to all CPUs running the mm
4. Each CPU receives IPI, executes:
     MCR p15, 0, r0, c8, c7, 1   @ TLBIMVA (flush local TLB)
     DSB
5. Each CPU sends ACK
6. CPU0 waits for all ACKs
7. Only after ALL CPUs ACK: CPU0 can safely free the physical page

ARM vs ARM64 shootdown:
  ARM32:  Software IPI (slow, O(nr_cpus) overhead)
  ARM64:  TLBI IS (Inner Shareable) — single instruction broadcasts to all CPUs
          in shareability domain via hardware fabric
```

### 5.3 Linux SMP TLB Flush Implementation

```c
/* arch/arm/mm/flush.c */

void flush_tlb_page(struct vm_area_struct *vma, unsigned long uaddr)
{
    struct mm_struct *mm = vma->vm_mm;

    if (tlb_ops_need_broadcast()) {
        /* SMP: send IPI to all CPUs with this mm active */
        on_each_cpu_mask(mm_cpumask(mm), ipi_flush_tlb_page,
                         (void *)uaddr, 1 /* wait for completion */);
    } else {
        /* UP or CPU that's the only one with this mm */
        local_flush_tlb_page(vma, uaddr);
    }
}

static void ipi_flush_tlb_page(void *arg)
{
    unsigned long va = (unsigned long)arg;
    local_flush_tlb_page_current(va);
}

static inline void local_flush_tlb_page_current(unsigned long va)
{
    /* Flush entry from local TLB (current ASID) */
    asm("mcr p15, 0, %0, c8, c7, 1" : : "r" (va));
    dsb(ish);
}
```

### 5.4 TLB Flush Batching (mmu_gather)

```c
/*
 * munmap() flushes TLB only ONCE at the end, not per-page.
 * Uses mmu_gather to batch PTEs, then single flush.
 */

/* Start of range unmap */
tlb_gather_mmu(&tlb, mm, start, end);

/* For each page: unmap PTE, record in tlb batch */
unmap_page_range(&tlb, vma, start, end, ...);
    /* calls tlb_remove_page() per page — batches PFNs */

/* End: flush TLB ONCE for entire range, then free pages */
tlb_finish_mmu(&tlb, start, end);
    /* → flush_tlb_range() → IPI with [start,end] range */
    /* → then batch-free all collected physical pages */

/* Why batch? */
/* Each IPI has overhead ~5-10μs. */
/* 1000 pages unmapped → 1 IPI vs 1000 IPIs = huge win */
```

---

## 6. TLB Lockdown

> **TLB Lockdown** is implementation-defined in ARMv7-A. It allows pinning critical entries into the TLB so they are never evicted.

### 6.1 Cortex-A8 TLB Lockdown

```assembly
/* Cortex-A8: 8 lockable entries in I-TLB, 8 in D-TLB */

/* Procedure to lock a TLB entry:
 * 1. Disable TLB lockdown eviction
 * 2. Perform a memory access to the target VA (triggers fill)
 * 3. Advance the lockdown pointer
 * 4. Enable lockdown protection
 */

/* Step 1: Set lockdown base index */
MOV r0, #LOCK_BASE
MCR p15, 0, r0, c10, c0, 0  @ Write I-TLB Lockdown Register (victim=base)

/* Step 2: Load VA to trigger TLB fill (entry goes to victim position) */
LDR r1, [r2]                 @ access VA r2 → TLB fill at victim index

/* Step 3: Advance victim pointer (so next access won't evict locked entry) */
ADD r0, r0, #(1 << 26)       @ increment victim
MCR p15, 0, r0, c10, c0, 0

/* Step 4: Locked entry now at position LOCK_BASE, victim=LOCK_BASE+1 */
/* Hardware will not evict entries with index < victim base */
```

### 6.2 Use Cases for TLB Lockdown

```
1. Real-time systems: Lock interrupt handler VA → guaranteed TLB hit
2. Security-critical code: Lock secure monitor VA → no eviction during SMC
3. Bootloader: Lock identity map entry before MMU enable
4. DSP cores (Qualcomm Hexagon): Lock frequently-used buffers

WARNING: Reduces available TLB entries for normal code
         Over-locking degrades overall performance significantly
         Most Linux kernels do NOT use TLB lockdown
```

---

## 7. Linux Kernel TLB API

### 7.1 Core TLB Flush Functions

```c
/* Flush ALL user-space TLB entries across all CPUs */
flush_tlb_all(void)
    → MCR p15, 0, r0, c8, c7, 0 on all CPUs (TLBIALL)
    → Used: ASID generation rollover, major mapping changes

/* Flush all TLB entries for a given mm (by ASID) */
flush_tlb_mm(struct mm_struct *mm)
    → MCR p15, 0, asid, c8, c7, 2 (TLBIASID) on all CPUs running mm
    → Used: exit_mm(), execve()

/* Flush single page for a given mm */
flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
    → MCR p15, 0, addr, c8, c7, 1 (TLBIMVA) on all CPUs running mm
    → Used: mprotect(), do_wp_page(), try_to_unmap()

/* Flush VA range for a given mm */
flush_tlb_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
    → Iterates pages if range small, else flush_tlb_mm()
    → Used: munmap(), mremap()

/* Flush kernel TLB entry (no ASID needed, global) */
flush_tlb_kernel_page(unsigned long kaddr)
    → TLBIMVAA (all ASIDs) — kernel mappings are nG=0 (global)
    → Used: vmalloc changes, ioremap

/* Flush kernel VA range */
flush_tlb_kernel_range(unsigned long start, unsigned long end)
    → Used: module loading/unloading, vmalloc_sync
```

### 7.2 __flush_tlb_entry() — Macro-Level Detail

```assembly
/* arch/arm/mm/tlb-v7.S */

ENTRY(__cpu_flush_user_tlb_range)
    /* r0=start, r1=end, r2=vma */
    vma_vm_mm r3, r2            @ r3 = vma->vm_mm
    mmid r3, r3                 @ r3 = mm->context.id (ASID in [7:0])
    dsb ish
1:  ALT_SMP(mcr p15, 0, r0, c8, c7, 1)   @ TLBIMVA (SMP: unified, current ASID)
    ALT_UP(mcr  p15, 0, r0, c8, c6, 1)   @ DTLBIMVA (UP: data TLB only)
    ALT_SMP(mcr p15, 0, r0, c8, c5, 1)   @ ITLBIMVA (SMP: instruction TLB)
    add r0, r0, #PAGE_SIZE
    cmp r0, r1
    blo 1b
    dsb ish
    mov pc, lr
ENDPROC(__cpu_flush_user_tlb_range)
```

---

## 8. TLB and Cache Interaction

### 8.1 VIVT Cache (Cortex-A8) — TLB Flush Must Precede Cache Flush

```
VIVT = Virtually Indexed, Virtually Tagged

Problem: Cache uses VA tags → two different VAs that map same PA
         have separate cache lines. On context switch:
           Old process VA=0x1000 → PA=0x80001000 cached at set X
           New process VA=0x1000 → PA=0x80002000 → cache MISS (good)
           But:
           New process VA=0x2000 → PA=0x80001000 → may HIT old cache line!
           (ALIAS: two VAs → same PA, same cache set, different tags)

Required order on context switch (VIVT):
  1. flush_cache_mm(prev)    ← flush all cache entries for prev process
  2. flush_tlb_mm(prev)      ← flush TLB entries for prev ASID
  3. switch ASID + TTBR0
  
Cortex-A9+ uses PIPT L1 → no aliasing, step 1 unnecessary
```

### 8.2 Cache Must Be Flushed Before TLB Invalidate (Writeback)

```
When unmapping a dirty page (write-back cache):

WRONG ORDER:
  1. Invalidate TLB entry
  2. Free physical page
  3. Dirty cache line evicted → writes to freed page → CORRUPTION

CORRECT ORDER:
  1. flush_cache_page() ← writeback dirty cache lines to PA
  2. Invalidate TLB entry (MCR TLBIMVA)
  3. DSB ← ensure both complete
  4. Free physical page (safe now)
```

---

## 9. Performance Analysis and Profiling

### 9.1 TLB Miss Rate Measurement

```bash
# ARM PMU events for TLB misses (Cortex-A9):
# Event 0x02: Instruction micro TLB miss
# Event 0x03: Data micro TLB miss  
# Event 0x05: Data TLB refill (L2 TLB miss → page walk)
# Event 0x04: Instruction TLB refill

# Using perf on Linux:
perf stat -e r02,r03,r04,r05 ./my_workload

# ARM Streamline (DS-5): hardware PMU events
# Cortex-A9 TLB events:
#   ARM_CORTEX_A9_ITLB_MISS
#   ARM_CORTEX_A9_DTLB_MISS
```

### 9.2 TLB Thrashing Scenarios

```
Scenario 1: Working set larger than TLB coverage
  TLB covers: 32 entries × 4KB = 128KB
  Application working set: 512KB
  → Every 128KB of access → full TLB eviction
  Fix: Use huge pages (1MB sections → 1 TLB entry per MB)

Scenario 2: Too many process switches (ASID pressure)
  System with 256+ tasks → ASID rollover every context switch
  → Full TLB flush each time → 100% TLB miss storm
  Fix: Reduce number of active processes, or migrate to ARM64 (16-bit ASID)

Scenario 3: Kernel vmalloc fragmentation
  Many small vmalloc allocations → many 4KB TLB entries
  → vmalloc_fault on every CPU for each new vmalloc
  Fix: Use physically contiguous kmalloc where possible

Scenario 4: misaligned mmap (cache + TLB coloring)
  VA[13:12] ≠ PA[13:12] on VIPT set-associative cache
  → Forces cache aliases → extra flushes
  Fix: kernel maps user pages with VA = PA at low bits (color matching)
```

### 9.3 TLB Coverage Optimization

```c
/* Using huge pages to reduce TLB pressure */

/* User space: use madvise with MADV_HUGEPAGE (transparent huge pages) */
madvise(addr, size, MADV_HUGEPAGE);

/* Kernel: use huge pages for kernel direct map */
/* In create_mapping():
 *   If region is 1MB aligned and cacheable → use section descriptor
 *   32 TLB entries × 1MB = 32MB coverage per TLB
 *   vs
 *   32 entries × 4KB = 128KB coverage per TLB (8x worse)
 */

/* Qualcomm SoC practice: map SoC peripheral registers with 1MB sections */
/* → One TLB entry per peripheral region instead of 256 × 4KB entries */
```

---

## 10. Cortex-A Series TLB Specifics

### 10.1 Comparison Table

| Core | ITLB | DTLB | L2 TLB | ASID Bits | Cache |
|------|------|------|---------|-----------|-------|
| Cortex-A5  | 8 FA  | 8 FA  | 128 4-way | 8 | VIPT L1 |
| Cortex-A8  | 32 FA | 32 FA | 128 4-way | 8 | VIPT L1 |
| Cortex-A9  | 32 FA | 32 FA | 128 4-way | 8 | PIPT L1 |
| Cortex-A15 | 32 FA | 32 FA | 512 4-way | 8 | PIPT L1 |
| Cortex-A17 | 32 FA | 32 FA | 256 4-way | 8 | PIPT L1 |

FA = Fully Associative

> **Qualcomm Krait** (ARMv7-A compatible, proprietary): Custom TLB with HLOS and secure TLB partitioning for TrustZone. ASID space split: lower ASIDs for HLOS, upper for Secure world.

### 10.2 Cortex-A15 L2 TLB (512 entries)

```
Cortex-A15 improvements over A9:
  - L2 TLB: 128 → 512 entries (4× more coverage)
  - Out-of-order page table walk
  - Simultaneous I+D TLB miss handling
  - Physical tags in L1 cache (PIPT) → no aliasing

Coverage:
  512 entries × 4KB = 2MB minimum (all small pages)
  512 entries × 1MB = 512MB (all sections) → typical kernel
```

---

## 11. Common TLB Bugs and Debugging

### 11.1 Missing DSB After PTE Write

```c
/* BUG: Race between CPU and hardware PTW */
void update_mapping_bug(pte_t *pte, phys_addr_t new_pa) {
    *pte = pfn_pte(new_pa >> PAGE_SHIFT, PAGE_KERNEL);
    /* BUG: No DSB here! */
    /* Hardware PTW might still read old PTE from cache */
    flush_tlb_kernel_page(va);
    /* Even TLBI here doesn't help — PTW reads old PTE and refills TLB */
}

/* FIX */
void update_mapping_correct(pte_t *pte, phys_addr_t new_pa) {
    *pte = pfn_pte(new_pa >> PAGE_SHIFT, PAGE_KERNEL);
    dsb(ishst);    /* ensure PTE write reaches memory before TLBI */
    flush_tlb_kernel_page(va);
    dsb(ish);      /* ensure TLBI complete before next access */
}
```

### 11.2 TLB Coherency After boot_ioremap

```c
/* BUG: Early ioremap used before MMU-enable sequence is complete */
/* The fix_to_virt() address may be in a stale TLB entry from identity map */

/* Correct early ioremap flush sequence */
set_pte_ext(pte, pfn_pte(pfn, type->prot_pte), 0);
local_flush_tlb_kernel_page(vaddr);  /* flush stale identity-map entry */
```

### 11.3 ASID Not Updated Before TTBR0

```assembly
/* BUG: TTBR0 updated BEFORE CONTEXTIDR → window with wrong ASID */
MCR p15, 0, r0, c2, c0, 0   @ TTBR0 = new PGD  ← NEW ASID NOT YET ACTIVE
                              @ CPU does speculative page table walk here
                              @ tagged with OLD ASID → stale TLB entry
ISB
MCR p15, 0, r1, c13, c0, 1  @ CONTEXTIDR = new ASID  ← TOO LATE

/* FIX: ASID first, then TTBR0 */
MCR p15, 0, r1, c13, c0, 1  @ CONTEXTIDR = new ASID
ISB                           @ ensure ASID active before TTBR0 write
MCR p15, 0, r0, c2, c0, 0   @ TTBR0 = new PGD
ISB                           @ pipeline flush
```

### 11.4 Debugging TLB Issues with Linux

```bash
# Check TLB flush events via ftrace
echo 1 > /sys/kernel/debug/tracing/events/tlb/enable
cat /sys/kernel/debug/tracing/trace

# ARM hardware PMU via perf:
perf stat -e dTLB-load-misses,iTLB-load-misses ./workload

# Check /proc/vmstat for TLB flush counts (if instrumented):
grep "tlb" /proc/vmstat

# Kernel symbol lookup for page fault handler:
addr2line -e vmlinux $(cat /proc/kallsyms | grep do_page_fault | awk '{print $1}')

# Decode DFSR fault:
# DFSR[10,3:0]: fault type
# 0b00101 = Section translation fault
# 0b00111 = Page translation fault  
# 0b01101 = Section permission fault
# 0b01111 = Page permission fault
```

---

## Summary: TLB Operation Decision Tree

```
Need to change a PTE?
  │
  ├─ Single kernel page?
  │    → flush_tlb_kernel_page(va)
  │
  ├─ Single user page (one process)?
  │    → flush_tlb_page(vma, va)
  │
  ├─ Range of user pages?
  │    → flush_tlb_range(vma, start, end)
  │
  ├─ All user pages for a process (exit)?
  │    → flush_tlb_mm(mm)
  │
  └─ All TLB entries (ASID rollover)?
       → flush_tlb_all()

Always wrap PTE write with:
  DSB before TLBI (ensure PTE visible to PTW)
  DSB after TLBI  (ensure TLB clean before access)
```

---

**Cross-References:**
- Doc 01: ASID (CONTEXTIDR), CP15 register map
- Doc 03: ASID generation counter, check_and_switch_context()
- Doc 05: Cache maintenance ordering with TLB operations
- Doc 08: DSB/ISB semantics required around TLB maintenance

---
**End of Document 4**
