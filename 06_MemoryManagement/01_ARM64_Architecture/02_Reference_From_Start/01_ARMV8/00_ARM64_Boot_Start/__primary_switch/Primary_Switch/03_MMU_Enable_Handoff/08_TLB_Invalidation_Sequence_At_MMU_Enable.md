# TLB Invalidation Sequence at MMU Enable

**Context:** Why TLB state matters at the exact moment the MMU is enabled  
**Reference:** `arch/arm64/kernel/head.S`, `__cpu_setup`, and `__enable_mmu`

---

## 0. Problem Statement

When the CPU executes `msr sctlr_el1, x0` (SCTLR_EL1.M = 0 → 1):
- Immediately all VAs are translated via TTBR0/TTBR1
- Any stale TLB entries that existed before this point are now used

**Question:** What TLB state exists at the moment the MMU is enabled?  
**Answer:** Depends entirely on the boot path. The kernel must guarantee safety
regardless of which path was taken.

---

## 1. TLB Invalidation in `__cpu_setup`

The invalidation sequence happens in `__cpu_setup`, before `__primary_switch`
is called. From `arch/arm64/kernel/head.S`:

```asm
// arch/arm64/mm/proc.S — __cpu_setup
tlbi    vmalle1         // Invalidate all TLB entries for EL1 (local CPU)
dsb     nsh             // Wait for TLB invalidation to complete (non-shareable)
isb                     // Instruction barrier — flush pipeline
```

### Why `vmalle1`?

`TLBI VMALLE1` = "TLB Invalidate by VA, all entries, EL1" — invalidates:
- All EL0 TLB entries (VA[63:48] interpreted as user space)
- All EL1 TLB entries (VA[63:48] interpreted as kernel space)
- Both TTBR0_EL1 and TTBR1_EL1 TLB entries
- Walk cache entries at all levels

It does NOT invalidate:
- EL2 TLB entries (separate regime, `TLBI ALLE2` handles those)
- EL3 TLB entries (separate regime, `TLBI ALLE3` handles those)

### Why Not `TLBI VMALLE1IS` (Inner Shareable broadcast)?

At the point `__cpu_setup` runs, this is the **boot CPU** executing alone.
Secondary CPUs are either not yet started or parked in `secondary_holding_pen`.

`VMALLE1` (non-broadcast) is sufficient because there is only one CPU. Using
`VMALLE1IS` would broadcast the invalidation to all CPUs in the inner shareable
domain — mostly harmless but slightly more expensive.

Secondary CPUs call `__cpu_setup` themselves and perform their own local TLBIs
when they start (`secondary_startup`).

### The DSB NSH After TLBI

`DSB NSH` = Data Synchronization Barrier, Non-Shareable scope.

The scope here is **Non-Shareable** because:
1. The TLBI itself was local (`vmalle1`, not `vmalle1is`)
2. We only need to ensure the local CPU's TLB is invalidated
3. NSH is cheaper than ISH (which would also drain the memory system within the
   inner shareable domain)

The DSB guarantees that the TLBI operation is **complete** before the next
instruction executes. Without it:
- The TLB might still be in the process of invalidation
- The immediately following instructions could see TLB entries that were
  supposed to be invalidated

### The ISB After DSB

The ISB ensures that subsequent instruction fetches occur after the TLB
invalidation is architecturally complete. Combined:

```
TLBI VMALLE1          ← Initiate TLB flush
DSB NSH               ← Ensure flush is complete (observable by memory system)
ISB                   ← Ensure flush is complete (observable by instruction pipeline)
```

---

## 2. Walk Cache Invalidation

Modern ARM64 CPUs implement walk caches (micro-TLBs that cache intermediate
page table walk results). These must also be flushed.

`TLBI VMALLE1` **does** invalidate walk caches on all ARM64 implementations.
The ARM ARM requires that TLBI operations that match a TLB entry also invalidate
the corresponding walk cache entries.

No separate walk cache invalidation instruction is needed.

---

## 3. TLB State at the Moment of MMU Enable

After `__cpu_setup` completes and `__primary_switch` is entered:

```
TLB state: EMPTY (completely invalidated by TLBI VMALLE1 in __cpu_setup)
```

When `msr sctlr_el1, x0` fires and the MMU is enabled:
1. First instruction fetch (after ISB) triggers a TLB miss
2. Hardware PTW walks TTBR0 → `__pi_init_idmap_pg_dir`
3. For `.idmap.text` VAs, the identity map returns PA = VA
4. TLB entry installed
5. Subsequent fetches in the same 2MB region hit the TLB

**This is the safest possible state**: An empty TLB means the PTW must walk
the tables for every access, but the tables are freshly built and correct.

---

## 4. Kexec: A Different Boot Path

In a kexec reboot (loading a new kernel without hardware reset):
- The hardware TLB may contain entries from the **previous kernel**
- Those entries map VAs to PAs that are meaningful in the OLD kernel's layout
- The new kernel has a different physical load address (KASLR randomization)

Without TLB invalidation in `__cpu_setup`, the new kernel would use stale TLB
entries pointing to the wrong physical addresses.

**`__cpu_setup` handles kexec** because it is always called, regardless of how
the kernel was booted. The `TLBI VMALLE1` flushes stale entries from the
previous kernel.

---

## 5. SMP Secondary CPU Boot and TLB Coherency

When secondary CPUs are brought up (via `secondary_startup`), they each call
`__cpu_setup` individually. This means:
- Each CPU flushes its own local TLBs
- After all CPUs have completed boot, the TLB state is coherent (all empty)
- The kernel then populates shared kernel mappings (via `swapper_pg_dir`)

**Why not share TLB entries during boot?**

Secondary CPUs cannot use TLB entries from the boot CPU because:
1. TLB entries are stored in per-CPU hardware structures
2. There is no hardware mechanism to "broadcast" TLB entries (only invalidations broadcast)
3. Each CPU must independently walk the page tables and populate its own TLBs

After boot, when both CPUs are running with the same `swapper_pg_dir` and `CnP=1`
in TTBR1, the hardware can optimize TLB sharing. But during boot, each CPU is independent.

---

## 6. ASID 0 and Global TLB Entries at Boot

At boot, all TLB entries installed during the early boot phase have:
- **ASID = 0** (from TTBR0/TTBR1 bits[63:48] = 0)
- **nG = 0** (global, from PTE bit[11] = 0 in kernel page tables)

**Global entries (nG=0):** Are valid regardless of the current ASID. When the
kernel later switches to a user process with ASID=1, the kernel's global TLB
entries remain valid. This is intentional — kernel mappings don't need to be
flushed on every user context switch.

**Non-global entries (nG=1):** Tagged with an ASID and only valid when the
current ASID matches. User page table entries use nG=1 to ensure isolation
between processes.

During boot, before any user process exists, all TLB entries are global (nG=0).

---

## 7. TLB Entry Anatomy

When the PTW walks the identity map after MMU enable and installs a TLB entry,
the hardware stores:

```
TLB entry structure (conceptual):
┌─────────────────────────────────────────────────────┐
│  Tag:                                               │
│    VMID      = 0 (EL1 regime has no VMID)          │
│    ASID      = 0 (from TTBR0 bits[63:48])          │
│    nG        = 0 (global — valid for all ASIDs)    │
│    VA        = bits[47:21] or [47:12] of input VA  │
│    TTL       = translation-table level indicator    │
├─────────────────────────────────────────────────────┤
│  Data:                                              │
│    PA        = bits[51:21] or [51:12] of output PA │
│    Attributes = MT_NORMAL (MAIR slot 4)             │
│    AP        = kernel RWX or RW depending on PTE   │
│    UXN, PXN  = from PTE                            │
│    S (SH)    = Inner Shareable                     │
│    AF        = 1 (access flag set)                 │
│    Contiguous = if contiguous hint used            │
└─────────────────────────────────────────────────────┘
```

---

## 8. Sequence Timeline

```
Time  Event
────  ──────────────────────────────────────────────────────────────────
T0    primary_entry called (physical execution, MMU off, TLB = unknown)
T1    __cpu_setup:
          TLBI VMALLE1          ← TLB flush initiated
          DSB NSH               ← TLB flush complete
          ISB                   ← Pipeline synchronized
          TLB state = EMPTY     ✓
T2    __primary_switch:
          bl __pi_early_map_kernel  ← page tables built (in memory)
          bl __enable_mmu:
              msr ttbr0_el1         ← identity map root loaded
              load_ttbr1            ← kernel map root loaded
              set_sctlr_el1:
                  DSB SY            ← all memory ops complete
                  msr sctlr_el1     ← MMU ENABLED
                  ISB               ← pipeline flush
          ret (via TTBR0 identity map)
T3    __primary_switched (VA, TTBR1):
          TLB state: filling from swapper_pg_dir
T4    start_kernel: TLB warming up for all kernel code paths
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The TLB (Translation Lookaside Buffer) in ARMv8-A is a hardware cache of VA->PA translations. It is organized per-exception-level and can be split (instruction TLB / data TLB) or unified. TLB entries include the PA, memory attributes from MAIR, permission bits (AP, UXN, PXN), and an ASID (Address Space ID) to distinguish user processes without full flushes. On reset the TLB state is UNDEFINED per the ARM Architecture Reference Manual (ARM ARM D5.10). Before enabling the MMU Linux executes TLBI VMALLE1IS (invalidate all EL1 TLB entries, inner-shareable) to ensure no stale entries exist.

### Kernel Perspective (Linux ARM64)
The TLB invalidation sequence in Linux at MMU enable time is:
  dsb ishst        // ensure all page-table writes are visible
  tlbi vmalle1     // invalidate all EL1 TLB entries (local)
  dsb ish          // wait for invalidation to complete
  isb              // flush instruction pipeline
This sequence appears in __enable_mmu (arch/arm64/kernel/head.S). After this, the MMU is enabled with a guaranteed clean TLB. During normal operation, Linux uses the mmu_notifier and tlb_gather interfaces to batch and shoot down TLB entries across CPUs.

### Memory Perspective (ARMv8 Memory Model)
The TLB is part of the ARMv8 memory system: it sits between the CPU's address generation unit and the physical memory bus. A TLB miss triggers a hardware page-table walk starting at the address stored in TTBR0_EL1 or TTBR1_EL1 depending on which VA range was accessed. Walk results are stored in the TLB with the ASID tag. The ARMv8 architecture guarantees that after a successful TLBI + DSB sequence, all subsequent TLB lookups will miss the invalidated entries and re-walk the current page tables.