# TLB State Before MMU Enable — Invariants, Hazards, and Flush Sequence

**Context:** The state of the TLB when `__primary_switch` calls `__enable_mmu`  
**Key question:** Why is `tlbi vmalle1` issued in `__cpu_setup` before arriving here?

---

## 0. What Is the TLB?

The Translation Lookaside Buffer (TLB) is a **hardware cache** for page table
walk results. When the CPU needs to translate VA → PA, it first checks the TLB:
- **TLB hit:** Translation is found immediately (1-2 cycles)
- **TLB miss:** Hardware page table walker (PTW) reads from memory (5-50+ cycles)

AArch64 CPUs typically have:

| TLB | Location | Scope |
|---|---|---|
| L1 I-TLB | L1 instruction side | Per-CPU, 32-64 entries, fully associative |
| L1 D-TLB | L1 data side | Per-CPU, 32-64 entries |
| L2 TLB (Unified TLB) | L2 level | Per-CPU, 512-4096 entries, set-associative |
| Walk cache | Intermediate page table caches | Per-CPU, caches PGD/PUD/PMD entries |

---

## 1. TLB Content at Boot — The Unpredictability Problem

After a hardware reset or power-on, the ARM Architecture Reference Manual
states that TLB contents are **UNPREDICTABLE**. This means:

- Some TLB entries might contain valid-looking but stale translations from a
  previous boot (e.g., after `kexec` warm reboot)
- Some entries might contain garbage that accidentally maps a VA to an
  incorrect PA
- Entries might have any ASID, any validity bit, any combination of attributes

If the MMU were enabled with stale TLB entries, the CPU might use a cached
(wrong) translation instead of walking the fresh page tables. This could cause:
1. The CPU to execute instructions from the wrong physical memory location
2. Data loads/stores to go to the wrong physical address
3. Privilege escalation if a stale kernel mapping is returned for a user-space VA

---

## 2. `__cpu_setup` TLB Invalidation — The Solution

`__cpu_setup` in `arch/arm64/mm/proc.S` performs the mandatory TLB flush
before returning:

```asm
// arch/arm64/mm/proc.S — end of __cpu_setup
tlbi    vmalle1         // Invalidate all stage 1 TLB entries for EL1
dsb     nsh             // Data Synchronization Barrier (non-shareable)
isb                     // Instruction Synchronization Barrier
```

### 2.1 `TLBI VMALLE1` Explained

`TLBI` = TLB Invalidate  
`VMALL` = Virtual Memory ALL (all VAs in the translation regime)  
`E1` = at EL1  

This instruction invalidates all TLB entries for:
- EL1 translations (TTBR0_EL1 and TTBR1_EL1)
- Any ASID
- Any VA
- The current PE (Processing Element = CPU core)

**It does NOT invalidate** EL2 or EL3 TLB entries. But the kernel only cares
about EL1 entries.

### 2.2 Why `DSB NSH` After `TLBI`

The `TLBI` instruction is a **broadcast** invalidation signal. However, the
actual completion of TLB invalidation is architecturally only guaranteed after
a **DSB** (Data Synchronization Barrier) that has completed.

```asm
tlbi    vmalle1    // Issue invalidation, does NOT wait for completion
dsb     nsh        // Wait until all TLB invalidations are complete
                   // nsh = non-shareable (for the boot CPU, this is sufficient)
```

Without `DSB NSH`, the CPU might proceed to write `SCTLR_EL1.M=1` while a
previous TLB entry is still being invalidated in the background. The MMU
could then use that still-valid-but-stale entry for an early translation.

### 2.3 Why `ISB` After `DSB`

```asm
isb    // Flush the CPU pipeline and instruction cache
```

The `ISB` (Instruction Synchronization Barrier) ensures:
1. All TLB maintenance operations are globally visible
2. The instruction pipeline is flushed — no speculatively fetched instructions
   from before the ISB are in-flight
3. The subsequent `msr tcr_el1, x9` (writing TCR_EL1) is the first instruction
   that can see the new TCR value

Without `ISB`, system register writes (`MSR`) might not be visible to the
very next instruction due to out-of-order execution.

---

## 3. TLB Architecture — Multiple Levels and Their State

### 3.1 L1 TLB (Per-CPU)

```
I-TLB: ~32 entries, fully associative, 1-cycle hit latency
D-TLB: ~32 entries, fully associative, 1-cycle hit latency
```

After `TLBI VMALLE1 + DSB NSH + ISB`, all L1 TLB entries are invalidated.

### 3.2 L2 Unified TLB (Per-CPU)

```
~1024-4096 entries, set-associative (varies by CPU implementation)
Used for both instruction and data accesses
~4-8 cycle hit latency
```

Also invalidated by `TLBI VMALLE1`.

### 3.3 Translation Walk Cache (Per-CPU)

Cortex-A and Neoverse CPUs implement a **page table walk cache** that caches
intermediate-level page table descriptors (PGD→PUD pointers, PUD→PMD pointers).
This is also invalidated by `TLBI VMALLE1`.

---

## 4. ASID — Address Space Identifier

### 4.1 What ASID Does

The ASID is a tag in each TLB entry that identifies **which address space** the
entry belongs to. TLB entries with ASID = 3 (for process 3) are not returned
for translations for ASID = 7 (process 7), even if the VA is the same.

This allows the kernel to **avoid full TLB flushes** on context switches —
just change `TTBR0_EL1` to point to the new process's page tables (with the
new ASID embedded in TTBR0[63:48]), and the hardware automatically ignores
old TLB entries from other ASIDs.

### 4.2 ASID at Boot

At boot time, both `TTBR0_EL1` and `TTBR1_EL1` have ASID = 0. All TLB
entries installed during boot (by the PTW during `__pi_early_map_kernel`)
are associated with ASID 0.

```
TTBR0_EL1 = physical_addr_of_idmap << 1   | ASID[15:8]=0 | ASID[7:0]=0
// bits[63:48] = ASID = 0
// bits[47:1]  = PA >> 1 (for 48-bit PA) or other encoding
// bit[0]      = CnP (Common not Private) = 0 at boot
```

### 4.3 Global Entries (nG=0)

Kernel PTEs have `nG=0` (nG = **n**ot **G**lobal, active-low naming):

```
nG = 0  → Global entry: NOT tagged with ASID; survives ALL ASID changes
nG = 1  → Non-global: tagged with current ASID; removed on ASID switch
```

Kernel mappings are **global** (`nG=0`) because they must be accessible from
every process's address space (the kernel is always mapped in TTBR1 regardless
of which user process is running).

Only user-space PTEs have `nG=1`.

---

## 5. CONTEXTIDR_EL1 — Process ID Register

`CONTEXTIDR_EL1` is a 32-bit register that the Linux kernel uses to store the
current process's PID for debug and trace tools (ETM trace, PMU sampling).

At boot:
```asm
// Not explicitly set during __primary_switch
// Value = 0 after __cpu_setup's msr write of CONTEXTIDR_EL1, or leftover from bootloader
```

`CONTEXTIDR_EL1` is unrelated to TLB tagging on ARMv8. (On ARMv7, the ASID
was encoded in CONTEXTIDR; on ARMv8 it moved to TTBR.) However, trace tools
use it to identify which process generated each trace packet.

---

## 6. TLBI Broadcast vs Local Invalidation

### 6.1 `TLBI VMALLE1` (Local)

Invalidates TLB entries on the **current CPU only**. Used in `__cpu_setup`
because we are running single-threaded at boot — only one CPU is executing.

### 6.2 `TLBI VMALLE1IS` (Inner Shareable broadcast)

```asm
tlbi    vmalle1is    // Broadcast to ALL CPUs in Inner Shareable domain
dsb     ish          // Wait for completion across all CPUs
```

Used after the kernel modifies shared page tables (e.g., `ptep_clear_flush`,
`flush_tlb_mm`). This broadcasts the invalidation to every CPU in the same
cluster.

**Why `IS` is needed for SMP:** When CPU0 removes a mapping from `swapper_pg_dir`,
CPU1 might have a TLB entry pointing to the old mapping. Without `TLBI IS`,
CPU1 could continue using the stale entry — accessing freed memory or
unintended PA.

At boot (`__cpu_setup`), `TLBI VMALLE1` (non-IS) is sufficient because only
one CPU is running. Secondary CPUs run their own `__cpu_setup` before joining
the kernel, each doing their own local TLB invalidation.

---

## 7. What Happens If TLB Is Not Invalidated Before MMU-On

This is a critical failure scenario worth walking through for an interview:

**Scenario:** Kexec from a previous kernel on an ARM64 server. The old kernel
had `TTBR1_EL1` pointing to its `swapper_pg_dir` at PA `0x5000_0000`. The
new kernel is loaded at PA `0x4000_0000`. The old kernel's TLB entries map
the kernel VA range `0xFFFF_8000_xxxx_xxxx` → PA `0x5000_xxxx`.

**Without TLB flush:**
1. New kernel's `__cpu_setup` writes `TTBR1_EL1` to point to its own
   `reserved_pg_dir` at PA `0x4000_yyyy`.
2. `__enable_mmu` sets `SCTLR_EL1.M = 1`.
3. The `br x8` jump to `__primary_switched` fires a fetch for
   VA `0xFFFF_8000_yyyy_yyyy`.
4. L1 I-TLB has a **stale hit** for this VA → PA `0x5000_xxxx` (old kernel).
5. The CPU executes **old kernel code** at `0x5000_xxxx` — instant corruption.

**With proper `TLBI VMALLE1 + DSB + ISB`:**
1. All TLB entries cleared.
2. The `br x8` VA lookup misses TLB → hardware PTW reads from `swapper_pg_dir`
   (new kernel's tables) → correct PA → correct execution.

This is why the `TLBI` sequence in `__cpu_setup` is not optional — it is
mandatory for correctness on warm boots and kexec.

---

## 8. TLB State Timeline Summary

| Event | TLB State |
|---|---|
| Hardware reset / power-on | UNPREDICTABLE |
| Bootloader execution | May have arbitrary entries |
| `__cpu_setup` → `tlbi vmalle1` + `dsb` + `isb` | All EL1 entries invalidated |
| `__enable_mmu` writes TTBR0, TTBR1 | Empty TLB; TTBRs set |
| `set_sctlr_el1 x0` (MMU on) | Empty TLB; MMU active |
| First instruction fetch after MMU-on | L1 I-TLB miss → PTW → identity map entry installed |
| `bl __pi_early_map_kernel` return | More TLB entries for `.idmap.text` region |
| `br x8` → `__primary_switched` | I-TLB miss on kernel VA → PTW → `swapper_pg_dir` entry installed |

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