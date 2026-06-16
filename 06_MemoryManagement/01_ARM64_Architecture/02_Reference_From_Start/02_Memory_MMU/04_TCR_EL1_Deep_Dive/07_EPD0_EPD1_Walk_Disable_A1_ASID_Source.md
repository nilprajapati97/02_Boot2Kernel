# EPD0 / EPD1: TTBR Walk Disable; A1: ASID Source Selection

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. EPD0 — TTBR0 Walk Disable

```
TCR_EL1.EPD0 (bit[7]):
  0 = Normal operation: translation table walks for TTBR0 are permitted
  1 = TTBR0 translation table walks are DISABLED
      Any VA in the TTBR0 range generates a Translation Fault
      (ESR_EL1.DFSC = 0b000100 = Translation Fault, Level 0)

EPD = "Enable Page-table walk Disable" (confusing naming: EPD=1 means walks Disabled)
```

### Use Cases for EPD0=1

```
1. Thread State Preservation During Kernel-Only Contexts
   Some real-time kernels disable TTBR0 when running pure kernel threads
   that have no user space context. Any accidental user VA access faults
   immediately rather than silently accessing the previous process's memory.

2. Secure EL1 Stub Page Tables
   TrustZone secure world initialization may use a stub TCR with EPD0=1
   to prevent any non-secure TTBR0 walks from the secure world.

3. KPTI Trampoline (partial):
   During Meltdown mitigation (KPTI), when running in user space, the
   kernel uses a "trampoline" page table (tramp_pg_dir) that maps only
   the kernel entry code. User space uses normal TTBR0.
   On return to kernel, TTBR0 is set to a reserved empty table with
   EPD0=0 but all VA unmapped (different from EPD0=1 fault mechanism).
   Some implementations DO use EPD0=1 in specific KPTI configurations.

4. Debug/Testing:
   Boot-time checks: Set EPD0=1, attempt kernel boot, verify no user VA
   access occurs unexpectedly (catches missing kernel-space VA adjustments).
```

---

## 2. EPD1 — TTBR1 Walk Disable

```
TCR_EL1.EPD1 (bit[23]):
  0 = Normal operation: kernel VA walks (TTBR1) permitted
  1 = TTBR1 translation table walks are DISABLED
      Any kernel VA access → Translation Fault

EPD1=1 is extremely dangerous:
  Setting EPD1=1 makes the kernel itself inaccessible.
  The exception vector is mapped at kernel VA.
  Any fault (interrupt, page fault, etc.) would try to vector through
  the kernel exception table → Translation Fault → infinite fault loop.
  → System is effectively dead.

When EPD1=1 is actually used:
  - EL2 hypervisor stub page tables (before guest OS sets up its own TCR)
  - Microkernel designs where a separate core runs with no kernel map
  - Ultra-early boot before TTBR1 is configured (but must switch away quickly)
  - Test harnesses that verify EL0-only code with no kernel VA access

Linux never sets EPD1=1 in normal operation.
```

---

## 3. A1 — ASID Source Selection

```
TCR_EL1.A1 (bit[22]):
  0 = ASID is taken from TTBR0_EL1[63:48]  ← Linux default
  1 = ASID is taken from TTBR1_EL1[63:48]

Context: ASID (Address Space ID) tags TLB entries for user processes.
  Non-global (nG=1) TLB entries are tagged with the current ASID.
  Only entries matching the current ASID are valid.
  This allows multiple processes' TLB entries to coexist.

Why two TTBR registers have ASID fields:
  Both TTBR0_EL1[63:48] and TTBR1_EL1[63:48] contain an ASID field.
  A1 selects which one is "active" (used for TLB lookup/fill).
  The other TTBR's ASID field is ignored for TLB tagging.

Linux A1=0 (ASID from TTBR0):
  ASID lives in TTBR0_EL1[63:48].
  At context switch: TTBR0 is updated with new process's ASID + page table.
  TTBR1 (kernel) ASID field is typically 0 (or same, for KPTI).
  
  Context switch: write new_asid | new_ttbr0_pa → TTBR0_EL1
  Atomically updates both ASID and page table base in one register write.
```

---

## 4. ASID Update During Context Switch

```c
// arch/arm64/mm/context.c (simplified)

void cpu_switch_mm(pgd_t *pgd, struct mm_struct *mm)
{
    u64 ttbr1 = read_sysreg(ttbr1_el1);   // current kernel TTBR1
    u64 asid  = atomic64_read(&mm->context.id) & ASID_MASK;
    u64 ttbr0 = phys_to_ttbr(__pa(pgd)) | (asid << 48);  // ASID in [63:48]

    // A1=0: ASID is in TTBR0_EL1[63:48]
    // Write TTBR0 atomically (ASID + PGD base in one instruction):
    write_sysreg(ttbr0, ttbr0_el1);
    isb();  // Ensure TTBR0 is visible before next walk
}

// For KPTI (Meltdown mitigation):
// When switching to user space, two TTBR0 values exist:
//   - tramp_pg_dir (minimal kernel-only map) while in kernel
//   - user process page table while in user space
// KPTI flips TTBR0 at exception entry/exit in entry.S
```

---

## 5. A1=1 Scenario: ASID from TTBR1

```
A1=1 means ASID comes from TTBR1_EL1[63:48].

When this is useful:
  If the OS wants to keep TTBR0 (user page table base) fixed and change
  only the ASID in TTBR1, this avoids updating TTBR0 just to change ASID.
  
  Microkernel scenario:
    TTBR0 holds a fixed shared library map (same PA for all processes)
    TTBR1 holds per-process kernel view with per-process ASID
    A1=1 allows ASID to be updated in TTBR1 without touching TTBR0

  ARM's recommendation: Use A1=0 (ASID in TTBR0) for standard Linux.
  TTBR0 changes anyway on every context switch (new user page table),
  so including ASID in TTBR0 costs nothing extra.

Linux does NOT use A1=1. A1=0 is hardcoded in Linux's TCR value.
```

---

## 6. ASID in TTBR0 vs TLB Operations

```
When A1=0 (Linux default):

TLB operations with ASID:
  TLBI ASIDE1IS, x0   // Invalidate all entries with ASID from x0[15:0]
    where x0[15:0] = ASID to invalidate

  TLBI VAE1IS, x0     // Invalidate by VA + ASID
    x0[63:48] = ASID
    x0[47:12] = VA[47:12]

  TLBI VMALLE1IS      // Invalidate all EL1 entries (ignores ASID)
    Used for kernel global entries (nG=0)
    Also clears all ASID-tagged entries (used on ASID rollover)

ASID rollover (when all 16-bit ASIDs exhausted):
  Linux clears ASID bitmap and generation counter
  Issues TLBI VMALLE1IS to flush ALL TLB entries
  Reassigns ASIDs from scratch with new generation
  This is infrequent (65536 ASIDs for 16-bit, 256 for 8-bit)
```

---

## 7. KPTI and EPD0/A1 Interaction

```
KPTI (Kernel Page Table Isolation) for Meltdown mitigation:

Two page tables per process:
  1. Full process page table (user + kernel): used while running in kernel
  2. Trampoline page table (user only + minimal kernel entry): used in user space

At EL0 (user space):
  TTBR1 = tramp_pg_dir (only exception vector + KPTI entry code mapped)
  TTBR0 = user process page table
  → Any speculative kernel read from user space hits unmapped → fault
  → This neutralizes Meltdown (speculative kernel reads can't get data)

At EL1 (kernel):
  TTBR1 = swapper_pg_dir (full kernel map)
  TTBR0 = user process page table (still valid for user accesses)

TTBR switch at exception entry (arch/arm64/kernel/entry.S):
  kernel_entry:
    msr TTBR1_EL1, x30   // switch from tramp_pg_dir to swapper_pg_dir
    isb
    // Now full kernel is accessible
    // Do NOT change EPD0 — user space TTBR0 stays valid for copy_to/from_user

EPD0 is NOT used in KPTI in mainline Linux.
KPTI relies on the trampoline page table having only minimal mappings,
not on EPD0 disabling all TTBR0 walks.
```

---

## 8. Interview Questions & Answers

**Q1: What does EPD0=1 do, and when would you set it?**

`EPD0=1` (TCR_EL1 bit[7]) disables page table walks for the TTBR0 (user) VA range. Any access to a user VA generates a Translation Fault. Use cases include: (1) Debug/testing to verify kernel code never accidentally accesses user VAs when a user context is not expected; (2) Secure world stub page tables in TrustZone EL1 where TTBR0 access must be prevented; (3) Microkernel or RTOS designs where kernel-only threads run with no valid user page table. Linux mainline does not use EPD0=1 in normal operation.

**Q2: What is TCR_EL1.A1 and what does it control?**

`A1` (bit[22]) selects which TTBR register's ASID field is used as the active ASID for TLB tagging. `A1=0` (Linux default): ASID is taken from `TTBR0_EL1[63:48]`. `A1=1`: ASID is taken from `TTBR1_EL1[63:48]`. Linux uses `A1=0` because at every context switch, TTBR0 is updated with the new process's page table base address AND ASID as a single atomic register write (`TTBR0_EL1 = asid | pgd_pa`). This is efficient — one write simultaneously changes both the address space identifier and the page table root.

---

## 9. Quick Reference

| Field | Bit | Value | Meaning |
|---|---|---|---|
| EPD0 | [7] | 0 | TTBR0 walks enabled (normal) |
| EPD0 | [7] | 1 | TTBR0 walks disabled → Translation Fault |
| EPD1 | [23] | 0 | TTBR1 walks enabled (normal) |
| EPD1 | [23] | 1 | TTBR1 walks disabled → Translation Fault (danger!) |
| A1 | [22] | 0 | ASID from TTBR0_EL1[63:48] (Linux default) |
| A1 | [22] | 1 | ASID from TTBR1_EL1[63:48] |

| Register | ASID field | PGD base |
|---|---|---|
| TTBR0_EL1 | [63:48] | [47:0] (trimmed by BADDR) |
| TTBR1_EL1 | [63:48] | [47:0] |
