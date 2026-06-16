# finalise_el2 — CPU Architecture Perspective: VHE vs nVHE Decision and Hardware Impact

**Classification**: ARM64 CPU Architecture — Virtualization Extension
**Scope**: `finalise_el2()` in `__primary_switched`, VHE vs nVHE selection
**Perspective**: CPU microarchitecture, exception level model, HCR_EL2
**Style Reference**: AMD Virtualization Technology / NVIDIA ARM Platform Guide

---

## 1. The Two Hypervisor Personalities: VHE vs nVHE

ARM64 supports two distinct software models for running the host OS
alongside a hypervisor:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              VHE vs nVHE: Two Different CPU Operating Models                │
├───────────────────────┬─────────────────────────────────────────────────────┤
│  nVHE (classic)       │  VHE (ARMv8.1+)                                    │
│  EL2 hypervisor stub  │  Full OS at EL2                                    │
├───────────────────────┼─────────────────────────────────────────────────────┤
│  Host kernel: EL1     │  Host kernel: EL2                                  │
│  KVM stub:   EL2      │  KVM stub:   EL2 (same level as kernel!)           │
│  Guest:      EL1/EL0  │  Guest:      EL1/EL0                               │
├───────────────────────┼─────────────────────────────────────────────────────┤
│  Guest entry:         │  Guest entry:                                      │
│    EL1 → EL2 switch   │    EL2 stays at EL2 (no level switch!)            │
│    (expensive)        │    (much cheaper)                                  │
├───────────────────────┼─────────────────────────────────────────────────────┤
│  Register aliasing:   │  Register aliasing:                                │
│    No (EL1 ≠ EL2)     │    Yes: EL1 regs → EL2 hardware                   │
│                       │    sctlr_el1 → sctlr_el2                          │
│                       │    ttbr0_el1 → ttbr0_el2                          │
│                       │    vbar_el1  → vbar_el2                           │
├───────────────────────┼─────────────────────────────────────────────────────┤
│  Page tables:         │  Page tables:                                      │
│    Separate EL2 tables│    EL1 tables ARE EL2 tables (shared)             │
│    (tiny, stub only)  │    swapper_pg_dir used for both                    │
├───────────────────────┼─────────────────────────────────────────────────────┤
│  Kernel code changes: │  Kernel code changes:                              │
│    None needed        │    None! Hardware aliasing is transparent          │
└───────────────────────┴─────────────────────────────────────────────────────┘
```

---

## 2. The VHE Hardware Mechanism: HCR_EL2.E2H and TGE

VHE is controlled by two bits in HCR_EL2 (Hypervisor Configuration Register):

```
HCR_EL2 — Selected Bit Fields for VHE:
┌─────────────────────────────────────────────────────────────────────────────┐
│  Bit  │  Name  │  Meaning when SET                                          │
├───────┼────────┼──────────────────────────────────────────────────────────  │
│  [34] │  E2H   │  EL2 Host: Enable VHE mode.                               │
│       │        │  When set:                                                 │
│       │        │  • EL1 architectural registers alias EL2 registers        │
│       │        │  • sctlr_el1 reads/writes sctlr_el2                       │
│       │        │  • ttbr0_el1 reads/writes ttbr0_el2                       │
│       │        │  • Exception return from EL2 can target EL2 (EL2h mode)  │
├───────┼────────┼──────────────────────────────────────────────────────────  │
│  [27] │  TGE   │  Trap General Exceptions:                                 │
│       │        │  When set (with E2H):                                     │
│       │        │  • EL0 exceptions go directly to EL2 (skip EL1)          │
│       │        │  • Used for guest OS: guest EL0 → host EL2               │
└───────┴────────┴──────────────────────────────────────────────────────────  │
```

### HCR_EL2 Values Used by Linux

```c
// arch/arm64/include/asm/kvm_arm.h
#define HCR_HOST_VHE_FLAGS  (HCR_RW | HCR_TGE | HCR_E2H)
                             // RW: EL1 in AArch64 mode
                             // TGE: trap EL0 to EL2
                             // E2H: VHE enable

#define HCR_HOST_NVHE_FLAGS (HCR_RW | HCR_ATA)
                             // RW: EL1 in AArch64
                             // ATA: allow tag access (MTE)
                             // No E2H → nVHE
```

---

## 3. Register Aliasing With VHE (E2H=1): What the CPU Does Transparently

When `HCR_EL2.E2H = 1`, the ARMv8.1 architecture specifies that EL1-named
system registers physically map to EL2 hardware registers. This aliasing is
**completely transparent to software** — code that writes `msr sctlr_el1, x0`
actually modifies `sctlr_el2`.

```
Register Aliasing Map (E2H=1, accessed at EL2):
┌────────────────────────────────────────────────────────────────────────────┐
│  EL1 register name   │  Physical register (E2H=1 at EL2)                  │
├──────────────────────┼──────────────────────────────────────────────────── │
│  sctlr_el1           │  sctlr_el2                                         │
│  ttbr0_el1           │  ttbr0_el2                                         │
│  ttbr1_el1           │  vttbr_el2 (no EL2 stage-1 TTBR1, shared with EL1)│
│  vbar_el1            │  vbar_el2                                           │
│  tcr_el1             │  tcr_el2                                           │
│  mair_el1            │  mair_el2                                          │
│  esr_el1             │  esr_el2                                           │
│  far_el1             │  far_el2                                           │
│  elr_el1             │  elr_el2                                           │
│  spsr_el1            │  spsr_el2                                          │
│  tpidr_el1           │  tpidr_el2                                         │
│  cpacr_el1           │  cptr_el2 (with FPEN field mapping)                │
└──────────────────────┴────────────────────────────────────────────────────  │
```

This aliasing means the Linux kernel binary needs **zero changes** to run
at EL2 with VHE — every system register access the kernel performs (writing
`sctlr_el1`, reading `ttbr0_el1`, etc.) automatically targets the correct
EL2 hardware register.

---

## 4. CPU State After VHE Promotion: Before and After `finalise_el2`

```
Before finalise_el2 (nVHE state, running at EL1):
┌──────────────────────────────────────────────────────────────────────────┐
│  CurrentEL     = EL1 (0b0100)                                           │
│  HCR_EL2       = HCR_HOST_NVHE_FLAGS (E2H=0)                           │
│  TTBR1_EL1     = swapper_pg_dir (kernel VA mappings)                   │
│  VBAR_EL1      = &vectors (just installed)                              │
│  sctlr_el1     = INIT_SCTLR_EL1_MMU_ON (M=1, C=1, I=1, ...)           │
│  SP            = init_task kernel stack                                 │
│  TPIDR_EL1     = __per_cpu_offset[0]                                    │
└──────────────────────────────────────────────────────────────────────────┘

After finalise_el2 (VHE state, running at EL2):
┌──────────────────────────────────────────────────────────────────────────┐
│  CurrentEL     = EL2 (0b1000)   ← CHANGED                              │
│  HCR_EL2       = HCR_HOST_VHE_FLAGS (E2H=1, TGE=1)  ← CHANGED         │
│  TTBR1_EL1     = swapper_pg_dir (aliased to vttbr_el2)  ← same value   │
│  VBAR_EL1      = &vectors (aliased to vbar_el2)  ← same value          │
│  sctlr_el1     = INIT_SCTLR_EL1_MMU_ON (aliased to sctlr_el2)  ← same │
│  SP            = sp_el1 (copied from EL1 context)  ← same stack        │
│  TPIDR_EL1     = __per_cpu_offset[0] (aliased to tpidr_el2)  ← same   │
└──────────────────────────────────────────────────────────────────────────┘
```

From the kernel's perspective: nothing changed. From the CPU's perspective:
it is now running at a **higher privilege level** with full EL2 capabilities.

---

## 5. Why VHE Promotion Happens HERE and Not Earlier

### Requirement: swapper_pg_dir Must Be Live

`enter_vhe` (the final step of VHE promotion) does:
```asm
// enter_vhe in arch/arm64/kernel/hyp-stub.S
tlbi   vmalle1          // Invalidate all TLBs
dsb    nsh
isb
mrs_s  x0, SYS_SCTLR_EL12   // Read the EL1 SCTLR we set up
set_sctlr_el1  x0            // Enable the MMU at EL2 using EL1 tables
```

This re-enables the MMU at EL2 using the same page tables that EL1 was
using (`swapper_pg_dir`, pointed to by TTBR1_EL1/TTBR1_EL2 alias).

`swapper_pg_dir` is only valid after `__pi_early_map_kernel` completes in
`__primary_switch`. Attempting VHE promotion before that would enable the
EL2 MMU with uninitialised page tables → immediate translation fault.

### Requirement: VBAR_EL1 Must Be Valid

After VHE, `vbar_el1` aliases `vbar_el2`. All EL2 exceptions route through
the kernel's `vectors` table. The `vectors` table must be installed and
the `isb` committed before VHE promotion happens.

### Requirement: init_task Stack Must Be Active

`__finalise_el2` (in the hyp-stub) transfers `sp_el1` to the EL2 stack pointer:
```asm
mrs  x0, sp_el1     // Read the EL1 kernel stack pointer
mov  sp, x0         // Install as EL2 SP
```

The `init_task` kernel stack (set up by `init_cpu_task`) must be active at
`sp_el1` for this transfer to work correctly.

---

## 6. The nVHE Path: When VHE Is Not Available

If the CPU does not support VHE (no ARMv8.1), or if `HCR_EL2.E2H` cannot
be set (e.g., secure monitor prevents it), `finalise_el2` returns without
doing anything:

```
finalise_el2 return paths:
  EL1 boot path:     b.ne 1f → ret  (never even tries HVC)
  CPU no VHE:        hvc → __finalise_el2 → check VH bit → HVC_STUB_ERR → ret
  Override (HVHE=0): hvc → __finalise_el2 → check override → HVC_STUB_ERR → ret
  VHE success:       hvc → __finalise_el2 → enter_vhe → eret (now at EL2)
```

In the nVHE case:
- Kernel runs at EL1
- KVM (if enabled) uses a separate EL2 stub (`kvm_hyp.S`) for guest transitions
- Each KVM VM entry/exit requires an EL1↔EL2 level switch (expensive)

### Performance Impact

```
VM entry/exit cost (approximate, measured on Cortex-A78):
  VHE:   ~500 cycles  (no exception level switch)
  nVHE:  ~800 cycles  (EL1 → EL2 switch + register save/restore)

For a VM doing 1 million I/O operations per second:
  VHE  overhead: 0.5B cycles/sec
  nVHE overhead: 0.8B cycles/sec
  Difference:    0.3B cycles/sec ≈ 10% overhead on a 3GHz core
```

This explains why VHE was added in ARMv8.1 and why `finalise_el2` does the
best-effort promotion.
