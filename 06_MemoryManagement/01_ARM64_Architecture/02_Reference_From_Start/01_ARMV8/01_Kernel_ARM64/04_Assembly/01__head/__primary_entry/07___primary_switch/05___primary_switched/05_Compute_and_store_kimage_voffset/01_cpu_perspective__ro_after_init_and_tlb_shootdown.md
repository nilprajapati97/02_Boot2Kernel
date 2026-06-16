# kimage_voffset — CPU Perspective: __ro_after_init, TLB Shootdown, and Write Protection

**Classification**: ARM64 CPU + MMU Architecture — Memory Protection
**Scope**: `__ro_after_init` section, `mark_rodata_ro()`, TLB consistency
**Perspective**: CPU cache/TLB behavior, page table permission changes
**Style Reference**: AMD AGESA Memory Protection / NVIDIA Tegra MMU TRM

---

## 1. The `__ro_after_init` Section: Mechanism and Rationale

```c
// arch/arm64/mm/mmu.c
u64 kimage_voffset __ro_after_init;
EXPORT_SYMBOL(kimage_voffset);
```

`__ro_after_init` expands to:
```c
#define __ro_after_init  __section(".data..ro_after_init") __read_mostly
```

This places `kimage_voffset` into a dedicated linker section (`.data..ro_after_init`)
that is treated as **read-write during boot** and **read-only after `mark_rodata_ro()`**.

### Why Not `const`?

`const` would place the variable in `.rodata` which is **read-only from the
very start** — it's mapped with read-only page table entries before `primary_entry`
completes. `kimage_voffset` cannot be `const` because its value is computed
dynamically at runtime (based on KASLR offset) and written in `__primary_switched`.

```
Timeline:
  Boot start:    .data..ro_after_init → mapped RW
  __primary_switched writes kimage_voffset
  start_kernel runs ...
  mark_rodata_ro(): .data..ro_after_init → remapped RO
  Boot complete: kimage_voffset is permanently read-only
```

---

## 2. `mark_rodata_ro()` — The Page Table Permission Change

```c
// arch/arm64/mm/mmu.c
void mark_rodata_ro(void)
{
    unsigned long section_size;

    /*
     * mark .rodata as read only. Use __init_begin rather than __end_rodata
     * to cover NOTES and EXCEPTION_TABLE.
     */
    section_size = (unsigned long)__init_begin - (unsigned long)__start_rodata;
    update_mapping_prot(__pa_symbol(__start_rodata), (unsigned long)__start_rodata,
                        section_size, PAGE_KERNEL_RO);

    debug_checkwx();
}
```

For `__ro_after_init` specifically (`.data..ro_after_init` section):
```c
// The section is bounded by __start_ro_after_init and __end_ro_after_init
// in the linker script. mark_rodata_ro() uses update_mapping_prot() to
// change PTE permissions from PAGE_KERNEL (RW) to PAGE_KERNEL_RO (RO).
```

### ARM64 Page Table Permission Bits for `kimage_voffset`

```
Before mark_rodata_ro():
  PTE for kimage_voffset:
  ┌───────────────────────────────────────────────────────────────────────┐
  │  [63]  UXN=1  (EL0 execute-never)                                    │
  │  [54]  PXN=1  (EL1 execute-never — data, not code)                   │
  │  [7:6] AP=00  (EL1: RW, EL0: no access)  ← WRITE ALLOWED            │
  │  [1:0] valid=1, page=1                                               │
  └───────────────────────────────────────────────────────────────────────┘

After mark_rodata_ro():
  PTE for kimage_voffset:
  ┌───────────────────────────────────────────────────────────────────────┐
  │  [63]  UXN=1  (EL0 execute-never)                                    │
  │  [54]  PXN=1  (EL1 execute-never)                                    │
  │  [7:6] AP=10  (EL1: RO, EL0: RO)  ← WRITE FORBIDDEN                 │
  │  [1:0] valid=1, page=1                                               │
  └───────────────────────────────────────────────────────────────────────┘
```

Any write to `kimage_voffset` after `mark_rodata_ro()` triggers a
**Data Abort (translation fault, permission level 3)**. In the kernel, this
calls `do_mem_abort()` → `do_page_fault()` → `BUG()` (since it's in kernel
address space with no VMA). The system panics.

---

## 3. TLB Shootdown Protocol for Permission Changes

Changing PTE permission bits requires invalidating the TLB on ALL CPUs,
because stale TLB entries may still cache the old RW permission.
If a secondary CPU has a cached TLB entry for `kimage_voffset` with RW
permission, it could still write to the variable even after the PTE says RO.

### ARM64 TLB Shootdown Sequence

```c
// arch/arm64/mm/mmu.c: update_mapping_prot() calls:
flush_tlb_kernel_range(start, end);

// which expands to:
// 1. Issue TLBI (TLB Invalidate) instruction
// 2. DSB (Data Synchronization Barrier) to wait for completion
// 3. ISB (Instruction Synchronization Barrier) for prefetch coherency
```

On SMP systems with multiple online CPUs, `flush_tlb_kernel_range` issues
an **Inter-Processor Interrupt (IPI)** to each online CPU, requesting it to
execute the TLBI instruction on its own hardware TLB.

```
CPU0 (mark_rodata_ro):             CPU1, CPU2, ... (secondary CPUs):
  1. Update PTEs (AP=10, RO)
  2. DSB ISH (flush store buffer)
  3. Send IPI to all other CPUs  → receive IPI
                                   execute TLBI on own TLB
                                   send completion signal
  4. Wait for all completions   ← completion received
  5. DSB ISH (wait for all TLBIs)
  6. ISB
  → kimage_voffset is now RO on ALL CPUs
```

### The ARM Memory Model Requirement

ARM64's memory model (weakly ordered) requires:
1. PTE write must be **globally observable** before TLBI executes
   → `DSB ISH` after PTE write
2. All TLBI operations on all CPUs must **complete** before the protection
   is considered enforced → `DSB ISH` after TLBI IPI acknowledgments
3. **Any instruction fetched after the DSB** sees the new PTE → `ISB`

Skipping any of these steps creates a race window where a CPU might use
a stale RW TLB entry to write to `kimage_voffset`.

---

## 4. CPU Cache Perspective: PoU vs PoC for PTE Changes

When `mark_rodata_ro()` writes new PTEs into `swapper_pg_dir`, the updated
PTEs must be **visible to the hardware page table walker** on all CPUs.

```
Software (LSU):  str xzr, [pte_addr]    # Write new PTE with AP=10
                                         # Store goes to L1D cache initially

Hardware PTW:    Fetches PTE from memory on TLB miss
                 Sees: L1D? L2? L3? DRAM?

ARM64 guarantee: After DSB ISH, all data cache levels are consistent.
                 The hardware page table walker sees the new PTE.
```

ARM64 requires:
```asm
// After PTE write, before TLBI:
dsb  ish   // Ensure PTE write is visible to all observers (including PTW)
tlbi vmalle1is   // Invalidate TLBs (inner shareable domain, all ASIDs)
dsb  ish   // Wait for TLB invalidation to complete
isb        // Instruction fetch sees new permissions
```

The "inner shareable domain" (ISH) covers all CPUs within a cluster that
share caches. For most ARM64 platforms (including NVIDIA Tegra/Orin), all
application cores are in the same inner shareable domain.

---

## 5. NVIDIA-Style Engineering Note: Why __ro_after_init Matters for Security

```
┌─────────────────────────────────────────────────────────────────────────┐
│  SECURITY INVARIANT: kimage_voffset Must Be Immutable Post-Boot         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  kimage_voffset is used in:                                             │
│    virt_to_phys()  → DMA address for device drivers                    │
│    phys_to_virt()  → Kernel pointer from physical address              │
│    kexec setup     → New kernel load address computation               │
│    kdump vmcore    → Crash dump address reconstruction                 │
│                                                                         │
│  If an attacker can write kimage_voffset:                               │
│    → All virt_to_phys() conversions are wrong                          │
│    → DMA operations write to wrong physical addresses                  │
│    → kexec loads wrong kernel image → arbitrary code execution         │
│    → kdump produces incorrect crash dumps (forensic evasion)           │
│                                                                         │
│  __ro_after_init ensures no kernel module, driver, or exploit can       │
│  alter this value after the kernel boot sequence completes.             │
│                                                                         │
│  This is a kernel self-protection measure against:                      │
│   • Kernel module bugs that corrupt kernel data                        │
│   • TOCTOU races in driver initialization                              │
│   • Post-exploitation persistence via kimage_voffset manipulation      │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 6. AMD-Style Programmer's Note: The Spectre v2 / BTB Interaction

KASLR is partially defeated by Branch Target Buffer (BTB) side channels
(Spectre v2). An attacker running in userspace can:

1. Spray the BTB with known-address branches
2. Cause the kernel to speculatively execute branch predictions from user BTB entries
3. Use timing side channels to infer whether the speculative execution
   touched a specific kernel address

This attack works regardless of `kimage_voffset` being read-only — the
attacker doesn't need to **write** `kimage_voffset`, they just need to
**infer** the kernel base address.

Linux mitigates this via:
```
CONFIG_RANDOMIZE_BASE         — KASLR (first line of defense)
CONFIG_UNMAP_KERNEL_AT_EL0    — KPTI: kernel unmapped at EL0 (Meltdown)
CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY  — SPBP/CSV2 mitigations
retpoline / SB barrier        — Indirect branch speculation barriers
```

`kimage_voffset` being `__ro_after_init` is a defense-in-depth measure
that complements these mitigations, not a replacement.
