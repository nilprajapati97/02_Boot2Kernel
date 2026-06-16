# Linux arm64 Context Management: context.c Deep Dive

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, kernel engineers

---

## 1. Overview of arch/arm64/mm/context.c

```
File: arch/arm64/mm/context.c
Purpose: Manage ASID allocation and TLB flushing for ARM64 processes

Key data structures:
  
  static atomic64_t asid_generation;
    — Global ASID generation counter (upper bits = generation, lower bits = ASID value)
    — Atomic because accessed from multiple CPUs concurrently
    
  static DECLARE_BITMAP(asid_map, NUM_USER_ASIDS);
    — Bitmap tracking which ASIDs are currently in use
    — NUM_USER_ASIDS = (1 << asid_bits) - 1 (reserves ASID 0 for kernel)
    — 8-bit ASIDs: 255 user ASIDs; 16-bit: 65535 user ASIDs
    
  static DEFINE_PER_CPU(atomic64_t, active_asids);
    — Per-CPU: which ASID is currently active on this CPU
    — Used to detect "is this CPU's ASID still valid?"
    
  static DEFINE_PER_CPU(u64, reserved_asids);
    — Per-CPU: ASID reserved for this CPU (even when CPU is off or not running)
    — Prevents a CPU's ASID from being recycled while it's still "in use"
    — Used during CPU hotplug and context switches
    
  static cpumask_t tlb_flush_pending;
    — Bitmap of CPUs that need TLB flush after next ASID allocation

ASID encoding in mm->context.id:
  u64 format: [generation:48 bits][asid:16 bits]
  (or [generation:56 bits][asid:8 bits] for 8-bit ASIDs)
  
  This allows atomic comparison of both generation and ASID value together
  Example:
    asid_generation = 0x100 → generation=1, ASID bits=0x00 to 0xFF
    mm->context.id = 0x10042 → generation=1, ASID=0x42
    If asid_generation is now 0x200 (generation=2):
      mm->context.id generation (0x1xx) ≠ current generation (0x2xx) → ASID stale!
```

---

## 2. check_and_switch_context()

```c
/* arch/arm64/mm/context.c */
void check_and_switch_context(struct mm_struct *mm)
{
    unsigned long flags;
    unsigned int cpu;
    u64 asid, old_active_asid;

    if (system_supports_cnp())
        cpu_set_reserved_ttbr0();

    asid = atomic64_read(&mm->context.id);

    /*
     * Fast path: ASID generation matches current generation.
     * The mm's ASID is still valid → just switch.
     */
    if (!((asid ^ atomic64_read(&asid_generation)) >> asid_bits)
        && atomic64_xchg_relaxed(this_cpu_ptr(&active_asids), asid))
        goto switch_mm_fastpath;
    
    /* Slow path: ASID stale or first allocation */
    raw_spin_lock_irqsave(&cpu_asid_lock, flags);
    /* Reload generation after taking lock (another CPU may have rolled over) */
    asid = atomic64_read(&mm->context.id);
    
    if (!((asid ^ atomic64_read(&asid_generation)) >> asid_bits))
        goto switch_mm_slowpath_fasttrack;
    
    /* Need a new ASID */
    asid = new_context(mm);
    atomic64_set(&mm->context.id, asid);

switch_mm_slowpath_fasttrack:
    cpu = smp_processor_id();
    if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending))
        local_flush_tlb_all();
    
    atomic64_set(this_cpu_ptr(&active_asids), asid);
    raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

switch_mm_fastpath:
    arm64_apply_bp_hardening();
    cpu_switch_mm(mm->pgd, mm);
}
```

---

## 3. new_context() — ASID Allocation

```c
/* arch/arm64/mm/context.c */
static u64 new_context(struct mm_struct *mm)
{
    static u32 cur_idx = 1;
    u64 asid = atomic64_read(&mm->context.id);
    u64 generation = atomic64_read(&asid_generation);

    if (asid != 0) {
        u64 newasid = generation | (asid & ~ASID_MASK);
        
        /* Can we reuse the ASID with a new generation? */
        if (!__test_and_set_bit(asid2idx(newasid), asid_map)) {
            /* Yes — same ASID value, new generation */
            return newasid;
        }
    }
    
    /* Allocate a new ASID from the bitmap */
    asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
    if (asid != NUM_USER_ASIDS)
        goto set_asid;
    
    /* Bitmap full → need ASID rollover */
    generation = new_asid_generation(); /* increments asid_generation */
    flush_context();                    /* clears bitmap, sets tlb_flush_pending */
    
    /* Try again with fresh bitmap */
    asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);

set_asid:
    __set_bit(asid, asid_map);
    cur_idx = asid;
    return generation | idx2asid(asid);
}
```

---

## 4. flush_context() — ASID Generation Rollover

```c
/* arch/arm64/mm/context.c */
static void flush_context(void)
{
    int i;
    u64 asid;
    
    /* 
     * Clear the entire ASID bitmap: all ASIDs become available for reuse.
     * Reserve ASID 0 (used for early kernel boot, before ASID allocation).
     */
    bitmap_clear(asid_map, 0, NUM_USER_ASIDS);
    __set_bit(0, asid_map); /* Reserve ASID 0 */
    
    /* 
     * For each CPU:
     *   Save the currently-active ASID as "reserved" for this CPU.
     *   This CPU is currently running with an active ASID — it still needs it.
     *   Mark the ASID as "in use" in the new bitmap.
     */
    for_each_possible_cpu(i) {
        asid = atomic64_xchg_relaxed(&per_cpu(active_asids, i), 0);
        /*
         * If this CPU has an active ASID and the owner mm is valid:
         *   reserve_asid(asid) → mark it in the new bitmap
         *   so it's not recycled while this CPU is still using it
         */
        if (asid == 0)
            asid = per_cpu(reserved_asids, i);
        __set_bit(asid2idx(asid), asid_map);
        per_cpu(reserved_asids, i) = asid;
    }
    
    /*
     * Mark all CPUs as needing TLB flush.
     * When each CPU next runs check_and_switch_context():
     *   cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending) == true
     *   → local_flush_tlb_all() clears stale TLB entries
     */
    cpumask_setall(&tlb_flush_pending);
}
```

---

## 5. cpu_switch_mm() — Final Hardware Switch

```c
/* arch/arm64/include/asm/mmu_context.h */
static inline void cpu_switch_mm(pgd_t *pgd, struct mm_struct *mm)
{
    BUG_ON(pgd == swapper_pg_dir);
    
    cpu_set_reserved_ttbr0();  /* Briefly install reserved TTBR0 (zero page) */
    cpu_do_switch_mm(virt_to_phys(pgd), mm);  /* Install new TTBR0 */
}

/* arch/arm64/mm/proc.S */
SYM_TYPED_FUNC_START(cpu_do_switch_mm)
    mrs     x2, ttbr1_el1       /* Read current TTBR1 (kernel page table) */
    
    /* Pack ASID into TTBR0 bits[63:48] */
    bfi     x1, x0, #48, #16   /* x1[63:48] = ASID from mm->context.id */
    
    /*
     * KPTI: The ASID is split into user/kernel variants.
     * If KPTI enabled: TTBR0_user uses ASID, TTBR0_kernel uses ASID|USER_ASID_FLAG
     * (handled by alternative patching at runtime)
     */
    
    msr     ttbr0_el1, x1       /* Atomic: ASID + page table base installed together */
    isb
    ret
SYM_FUNC_END(cpu_do_switch_mm)
```

---

## 6. Key Design Decisions

```
1. Generation-based ASID encoding (mm->context.id = generation | asid):
   Allows O(1) stale ASID detection: compare upper bits of mm->context.id
   with upper bits of asid_generation. If different → stale.
   No per-ASID validity array needed.

2. Lazy TLB flush (tlb_flush_pending per-CPU bitmap):
   ASID rollover doesn't immediately flush all CPUs.
   Each CPU flushes its own TLB only when it next runs check_and_switch_context().
   "Lazy" = deferred flush until CPU actually needs to context switch.
   Avoids unnecessary cross-CPU IPIs for CPUs that aren't currently switching.

3. Atomic TTBR0 update (ASID + BADDR in one MSR):
   ARM64 guarantees that MSR TTBR0_EL1 is atomic — the ASID and page table base
   are installed simultaneously. No window where stale ASID + new PT or new ASID
   + stale PT could be active.
   
   Compare with non-atomic update (bad):
     MSR TTBR0_EL1_ASID, new_asid  ← between these two instructions,
     MSR TTBR0_EL1_BASE, new_base  ← ASID says "new process" but PT is still old!
   ARM64 avoids this by combining both in one 64-bit MSR.

4. ASID 0 reserved:
   Used during early boot before ASID allocation is active.
   Also used as "no ASID" for kernel-only mappings.
   Linux never allocates ASID 0 to user processes.
```

---

## 7. Interview Questions & Answers

**Q1: What is the purpose of the `reserved_asids` per-CPU variable in context.c?**

During an ASID rollover (`flush_context()`), we increment the generation counter and clear the `asid_map` bitmap to make all ASID values available for reuse. However, CPUs that are **currently running** (mid-context-switch or in kernel with a user process's TTBR0 loaded) still have a valid ASID in their TLB and TTBR0. If we immediately recycled that ASID for a different process, the CPU would see TLB entries for the OLD process when accessing the NEW process's pages — a security and correctness catastrophe.

`reserved_asids[cpu]` stores the ASID that was active on each CPU at rollover time. `flush_context()` reads `active_asids[cpu]`, moves it to `reserved_asids[cpu]`, and marks that ASID in the NEW bitmap so it's not recycled. Each CPU's "in-flight" ASID is preserved across the rollover. When that CPU next runs `check_and_switch_context()`, it will either reuse its reserved ASID (if the new generation happens to use the same value) or allocate a fresh one, after which the reserved ASID can be safely reused.

---

## 8. Quick Reference

| Variable | Type | Purpose |
|---|---|---|
| `asid_generation` | `atomic64_t` | Current global generation counter |
| `asid_map` | `BITMAP` | Which ASID values are currently allocated |
| `active_asids` (per-CPU) | `atomic64_t` | ASID currently loaded on this CPU |
| `reserved_asids` (per-CPU) | `u64` | ASID saved during rollover for safety |
| `tlb_flush_pending` | `cpumask_t` | CPUs needing TLB flush after rollover |
| `mm->context.id` | `atomic64_t` | Process ASID + generation (packed) |

| Function | Action |
|---|---|
| `check_and_switch_context()` | Fast/slow path ASID validation + switch |
| `new_context()` | Allocate new ASID, possibly trigger rollover |
| `flush_context()` | ASID rollover: clear bitmap, save reserved ASIDs |
| `cpu_switch_mm()` | Write ASID + TTBR0 to hardware |
