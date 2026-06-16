# finalise_el2 — ARM64 Perspective: Step-by-Step EL2 Transition Mechanics

**Classification**: ARM64 Exception Level Mechanics — VHE Promotion
**Scope**: `__finalise_el2` → `enter_vhe` exact instruction sequence
**Perspective**: ARM64 system register operations, SPSR/ELR manipulation
**Style Reference**: ARM Architecture Reference Manual / NVIDIA System Software Guide

---

## 1. Overview: The Three-Phase VHE Promotion

VHE promotion is split across three components:

```
Phase 1: finalise_el2() [arch/arm64/kernel/hyp-stub.S, EL1 code]
  → Eligibility check (booted at EL2? still at EL1?)
  → HVC #0 to trap into hyp-stub

Phase 2: __finalise_el2 [hyp-stub.S, EL2 code]
  → Hardware capability check (VHE supported? MMU off at EL2?)
  → MM state transfer (EL1 registers → EL2 registers)
  → SPSR patch to return to EL2h
  → Branch to enter_vhe

Phase 3: enter_vhe [hyp-stub.S .idmap.text, EL2 code]
  → TLB flush
  → Enable EL2 MMU
  → eret → back to caller (now at EL2)
```

---

## 2. Phase 1: `finalise_el2()` — The EL1 Caller

```asm
// arch/arm64/kernel/hyp-stub.S
SYM_FUNC_START(finalise_el2)
    // Gate 1: Did we boot at EL2?
    cmp    w0, #BOOT_CPU_MODE_EL2
    b.ne   1f              // No: return immediately, nothing to do

    // Gate 2: Are we still at EL1?
    mrs    x0, CurrentEL
    cmp    x0, #CurrentEL_EL1
    b.ne   1f              // No: unexpected (already at EL2?), return

    // Both gates passed: issue HVC to enter EL2
    mov    x0, #HVC_FINALISE_EL2
    hvc    #0              // Trap to EL2 hyp-stub

1:  ret
SYM_FUNC_END(finalise_el2)
```

### Why Gate 2 (CurrentEL Check)?

After VHE promotion, the kernel runs at EL2. If `finalise_el2` is called
again (e.g., CPU hotplug, suspend/resume), `CurrentEL` would read EL2.
Attempting another HVC from EL2 would trap to EL3 (or undefined behavior
if EL3 is not implemented) — not EL2. The gate prevents this.

### `hvc #0` — The Hypervisor Call

`hvc #0` is a synchronous exception that causes the CPU to:
1. Save return state into ELR_EL2, SPSR_EL2
2. Set PC to `VBAR_EL2 + 0x000` (synchronous exception from EL1 SP_EL1)
   Wait — actually:
   `hvc` from EL1 → VBAR_EL2 + 0x000 (synchronous exception from AArch64 EL1/EL0)
3. Change CurrentEL to EL2

The hyp-stub has installed its own vector table at VBAR_EL2 (done in
`init_el2` during `init_kernel_el`). The HVC vector dispatches based on
the immediate value (x0 = `HVC_FINALISE_EL2`).

---

## 3. Phase 2: `__finalise_el2` — The EL2 Implementation

```asm
// Running at EL2 now (entered via HVC)
SYM_CODE_START_LOCAL(__finalise_el2)
    finalise_el2_state    // Configure remaining EL2 control registers
```

`finalise_el2_state` is a macro that sets up:
- `HCR_EL2` with initial hypervisor configuration flags
- `CPTR_EL2` (disable trapping of FP/SIMD from EL2/EL1)
- `CNTHCTL_EL2` (counter access permissions)
- Various EL2 trap control registers

### Hardware Capability Check

```asm
    // Check: MMU must be OFF at EL2 (required for safe page table transfer)
    mrs    x1, sctlr_el2
    tbnz   x1, #0, 1f          // If SCTLR_EL2.M=1 (MMU on) → nVHE path
```

**Why must EL2 MMU be off?**

During VHE promotion, we copy page table base addresses from EL1 registers
(TTBR0_EL1, TTBR1_EL1) to their EL2 hardware equivalents. If the EL2 MMU
were already on (with some other page tables), disabling it temporarily
would be required — a complex, risky operation. The design decision is
simpler: VHE is only supported when EL2 starts with MMU off, which is
always the case here because `__enable_mmu` only enabled the EL1 MMU
(SCTLR_EL1.M), not SCTLR_EL2.M.

```asm
    // Check: CPU must support VHE (ARMv8.1)
    check_override id_aa64mmfr1 ID_AA64MMFR1_EL1_VH_SHIFT 0f 1f x1 x2
    // Reads ID_AA64MMFR1_EL1.VH field; if 0 → CPU has no VHE → nVHE path
```

```asm
    // Check: Software override (HVHE feature flag)
0:  adr_l  x1, arm64_sw_feature_override
    // If HVHE=0 in override flags → force nVHE
    cbz    x2, 2f    // HVHE=0 → skip VHE
1:  mov_q  x0, HVC_STUB_ERR
    eret             // Return to EL1 with error code in x0
```

---

## 4. The MM State Transfer: Moving EL1 Context to EL2

This is the most critical sequence in `__finalise_el2`. We are copying the
kernel's carefully constructed EL1 memory management state to EL2, so that
when we re-enable the MMU at EL2, it uses the same page tables.

```asm
2:  // VHE path begins
    mov_q  x0, HCR_HOST_VHE_FLAGS    // E2H | TGE | RW
    msr    hcr_el2, x0               // Enable VHE!

    // Transfer stack and per-CPU offset from EL1 to EL2
    mrs    x0, sp_el1                 // EL1 kernel stack pointer
    mov    sp, x0                     // EL2 SP = EL1 SP (same init_task stack)
    mrs    x0, tpidr_el1              // EL1 per-CPU offset
    msr    tpidr_el2, x0              // EL2 per-CPU = EL1 per-CPU

    // Transfer FP/SIMD access control
    mrs_s  x0, SYS_CPACR_EL12        // EL1's CPACR (FP access bits)
    msr    cpacr_el1, x0             // Install in EL2 CPACR (aliased after E2H=1)

    // Transfer exception vector table
    mrs_s  x0, SYS_VBAR_EL12         // EL1's VBAR (points to &vectors)
    msr    vbar_el1, x0              // Install in EL2 VBAR (aliased after E2H=1)
```

**Critical ordering**: `msr hcr_el2, x0` (with E2H=1) is written **first**.
After this instruction, the aliasing takes effect. All subsequent `msr
sctlr_el1` writes go to `sctlr_el2`, `msr vbar_el1` goes to `vbar_el2`, etc.

```asm
    // Transfer Translation Control Register
    mrs_s  x0, SYS_TCR_EL12          // EL1's TCR (T0SZ, T1SZ, granule, IPS...)
    msr    tcr_el1, x0               // → tcr_el2 (aliased)

    // Transfer page table bases
    mrs_s  x0, SYS_TTBR0_EL12        // EL1's TTBR0 (user/identity map base)
    msr    ttbr0_el1, x0             // → ttbr0_el2 (aliased)

    mrs_s  x0, SYS_TTBR1_EL12        // EL1's TTBR1 (= swapper_pg_dir)
    msr    ttbr1_el1, x0             // → vttbr_el2 (no direct EL2 TTBR1 equivalent)

    // Transfer memory attribute indirection register
    mrs_s  x0, SYS_MAIR_EL12         // EL1's MAIR (5 memory type encodings)
    msr    mair_el1, x0              // → mair_el2 (aliased)

    // If TCR2 supported (FEAT_TCR2):
    mrs    x1, REG_ID_AA64MMFR3_EL1
    ubfx   x1, x1, #ID_AA64MMFR3_EL1_TCRX_SHIFT, #4
    cbz    x1, .Lskip_tcr2
    mrs    x0, REG_TCR2_EL12         // Extended translation control
    msr    REG_TCR2_EL1, x0          // → tcr2_el2 (aliased)

    // If PIE (Permission Indirection Extension) supported:
    mrs    x0, REG_PIRE0_EL12        // EL0 PIR entries
    msr    REG_PIRE0_EL1, x0         // → EL2 equivalent
    mrs    x0, REG_PIR_EL12          // EL1 PIR entries
    msr    REG_PIR_EL1, x0           // → EL2 equivalent
```

---

## 5. SPSR Manipulation: Tricking `eret` to Return to EL2

At this point, `SPSR_EL2` and `ELR_EL2` were set when the `hvc` instruction
fired:
```
ELR_EL2  = return address (next instruction after `hvc #0` in EL1 code)
SPSR_EL2 = PSTATE at time of HVC (EL1h mode, i.e., PSTATE.M = 0b0101)
```

If we executed `eret` now, it would return to EL1. But we want to return
to EL2. The trick: **patch SPSR_EL2** to change the return mode:

```asm
    // Patch SPSR to return to EL2h instead of EL1h
    mrs    x0, spsr_el1          // Read current SPSR_EL2 value
                                 // (aliased to spsr_el1 after E2H=1)
    and    x0, x0, #~PSR_MODE_MASK   // Clear mode bits
    mov    x1, #PSR_MODE_EL2h        // EL2h = 0b1001 = EL2 using SP_EL2
    orr    x0, x0, x1
    msr    spsr_el1, x0          // Write patched SPSR_EL2

    b      enter_vhe             // Jump to the MMU re-enable sequence
```

```
Before SPSR patch:
  SPSR_EL2.M = 0b0101  (EL1h) → eret would go to EL1

After SPSR patch:
  SPSR_EL2.M = 0b1001  (EL2h) → eret will go to EL2 with SP_EL2
```

---

## 6. Phase 3: `enter_vhe` — MMU Re-enable at EL2

```asm
// .idmap.text section (must be identity-mapped!)
SYM_CODE_START_LOCAL(enter_vhe)
    // TLB flush before enabling MMU (mandatory ARM requirement)
    tlbi   vmalle1          // Invalidate all EL1 TLB entries
    dsb    nsh              // Wait for TLB invalidation
    isb                     // Instruction barrier

    // Enable EL2 MMU using the EL1-built page tables
    mrs_s  x0, SYS_SCTLR_EL12       // Read SCTLR_EL1 (= sctlr_el2 after E2H)
                                     // Has M=1 (MMU enable) already set
    set_sctlr_el1  x0                // Write SCTLR_EL2 with M=1
                                     // EL2 MMU is NOW ON
                                     // enter_vhe is identity-mapped → safe

    // Disable EL1 S1 MMU for cleanliness (not strictly needed since
    // we're no longer at EL1, but prevents confusion if EL1 is ever entered)
    mov_q  x0, INIT_SCTLR_EL1_MMU_OFF
    msr_s  SYS_SCTLR_EL12, x0       // Clear EL1 SCTLR (M=0)

    mov    x0, xzr                   // Return value = 0 (success)
    eret                             // Return to ELR_EL2 at EL2h
SYM_CODE_END(enter_vhe)
```

### Why `enter_vhe` Must Be in `.idmap.text`

When `set_sctlr_el1 x0` (which writes `sctlr_el2` after E2H=1) enables
the EL2 MMU, the **next instruction fetch** goes through the EL2 MMU.

At this moment:
- EL2 TTBR0 = identity map (physical == virtual for boot code)
- EL2 TTBR1 = swapper_pg_dir (kernel virtual mappings)

The current PC is the **physical address** of the `set_sctlr_el1` instruction
(we're in the hyp-stub at EL2, running from its physical address). For
execution to continue, the physical address of the next instruction must be
mapped via TTBR0 or TTBR1.

`enter_vhe` is placed in `.idmap.text`, which is identity-mapped (phys==virt).
So the physical address IS a valid virtual address via TTBR0. Execution
continues seamlessly.

If `enter_vhe` were in regular `.text`, its physical address would NOT be in
the identity map → immediate instruction fetch fault → system hang.

---

## 7. The Final `eret`: Landing Back in `__primary_switched` at EL2

```
State at eret:
  ELR_EL2  = address of instruction after `hvc #0` in finalise_el2()
           = `1: ret` (the function epilogue)
  SPSR_EL2 = (patched) EL2h mode

eret atomically:
  PC ← ELR_EL2   = address after hvc in finalise_el2()
  PSTATE ← SPSR_EL2 → CurrentEL = EL2, PSTATE.SP = 1

Execution resumes:
  finalise_el2() → ret → __primary_switched (now at EL2!)
  ldp  x29, x30, [sp], #16    // frame epilogue
  bl   start_kernel            // Enter C kernel at EL2
```

From `start_kernel`'s perspective, nothing changed — it's C code, it
doesn't know (or need to know) whether it runs at EL1 or EL2. All system
register accesses transparently use the EL2 hardware registers via aliasing.
