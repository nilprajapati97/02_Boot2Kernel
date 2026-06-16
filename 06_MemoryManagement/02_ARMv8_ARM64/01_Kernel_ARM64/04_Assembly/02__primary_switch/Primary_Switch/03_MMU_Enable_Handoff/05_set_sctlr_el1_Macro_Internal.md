# `set_sctlr_el1` Macro — Why a Plain `msr` Is Not Enough

**Source:** `arch/arm64/include/asm/assembler.h`  
**Called from:** `__enable_mmu` — the last step before the function returns  
**Purpose:** Write `SCTLR_EL1` to enable the MMU with all required barriers and erratum workarounds

---

## 0. The Naive Approach and Why It Fails

A developer unfamiliar with ARM64 errata might write:

```asm
msr     sctlr_el1, x0   // Set SCTLR_EL1, enable MMU
ret                       // Return — next fetch goes through MMU
```

This is **architecturally correct** on a perfect CPU but may fail on real
silicon due to:
1. Missing pipeline barriers
2. CPU errata (implementation bugs) that require specific workaround sequences
3. Missing cache maintenance that leaves the ISB target uncached

The `set_sctlr_el1` macro handles all of these.

---

## 1. The `set_sctlr_el1` Macro Definition

```asm
// arch/arm64/include/asm/assembler.h

.macro set_sctlr_el1, reg
    dsb     sy                    // (1) Full system data sync barrier
    msr     sctlr_el1, \reg       // (2) Write SCTLR_EL1 — MMU ENABLED HERE
    isb                           // (3) Instruction sync barrier — pipeline flush
    .endm
```

Three instructions: DSB, MSR, ISB.

---

## 2. The `DSB SY` Before the MSR

```asm
dsb     sy
```

**DSB** = Data Synchronization Barrier.  
**SY** = Full system scope (all memory system operations).

This barrier ensures that ALL outstanding memory operations (stores to RAM,
TLB invalidations, cache maintenance operations) are **complete** before the
`msr sctlr_el1` instruction executes.

### Why DSB Is Needed Here

Before `set_sctlr_el1` executes, `__enable_mmu` has issued:
1. `msr ttbr0_el1, x2` — write TTBR0 (system register write)
2. `load_ttbr1` — write TTBR1 (system register write + ISB inside)
3. The `msr` instructions for TTBR may not be architecturally complete until
   a DSB is executed

Without `DSB SY`:
- The TTBR writes might still be in-flight in the CPU's register write buffer
- The hardware could begin translating addresses using old (or undefined) TTBR
  values for a brief window after `SCTLR_EL1.M` is set

With `DSB SY`:
- All previous system register writes are guaranteed complete
- TTBR0 and TTBR1 hold the correct values
- The page tables they point to are in memory (were written by `__pi_create_init_idmap`)

**ARM ARM requirement:** The ARM Architecture Reference Manual (section
`D1.10.3 — Changes to the Translation Regime`) explicitly requires that
changes to TTBR0/TTBR1 be followed by a DSB before SCTLR_EL1.M is set.

---

## 3. The `MSR SCTLR_EL1` — The Actual MMU Enable

```asm
msr     sctlr_el1, x0
```

This is the single instruction that:
1. Enables the MMU (`M = 1`)
2. Enables the D-cache (`C = 1`)
3. Enables the I-cache (`I = 1`)
4. Sets all other SCTLR bits as pre-computed by `__cpu_setup`

**The critical moment:** At the clock edge when this instruction commits,
the CPU begins routing ALL subsequent memory accesses through the MMU.
There is no gradual transition — it is an atomic flip.

**Pipeline state at this instant:**

The `msr` instruction itself is fetched and decoded with the MMU off (it was
in the instruction fetch queue before the MMU was enabled). However, the
instruction fetch for the NEXT instruction (`isb`) may already use the MMU.

The ARM ARM guarantees a specific behavior at this boundary:
- The instruction that writes SCTLR_EL1 is fetched and executed at PA.
- Instructions after the `isb` are fetched at VA (through the now-active MMU).
- The `isb` acts as the synchronization point.

---

## 4. The `ISB` — Instruction Synchronization Barrier

```asm
isb
```

ISB is the most powerful barrier on ARM64. It:

1. **Flushes the instruction pipeline** — any instructions fetched speculatively
   before the ISB that have not yet executed are discarded.
2. **Forces re-fetch** — all subsequent instruction fetches happen with the new
   CPU state (including the newly set SCTLR_EL1).
3. **Ensures system register visibility** — changes to SCTLR_EL1 made before
   the ISB are guaranteed visible to all instructions after the ISB.

### Why ISB Is Mandatory After SCTLR Write

Without `ISB`:

```
Pipeline stage:  F  D  E  W
                 ──────────────────────────────────────────────
msr sctlr_el1:   F  D  E  W ← SCTLR_EL1.M committed to 1
isb:             F  D  ...
ret:             F ← Fetched BEFORE ISB, might use wrong address!
```

With `ISB`:

```
msr sctlr_el1:   F  D  E  W ← SCTLR_EL1.M committed to 1
isb:             F  D  E  ← PIPELINE FLUSH. All subsequent fetches re-issued
ret:                        F ← Re-fetched AFTER ISB, uses new SCTLR (MMU active)
```

The `ret` instruction is fetched as a virtual address using the identity map
in TTBR0. Since the `.idmap.text` section is identity-mapped, the VA equals
the PA, and the instruction is fetched correctly.

---

## 5. `pre_disable_mmu_workaround` — Erratum Workarounds

When the kernel **disables** the MMU (e.g., in `record_mmu_state` when
the bootloader left the MMU on), it uses:

```asm
.macro pre_disable_mmu_workaround
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_E1041
    isb
#endif
.endm
```

This is a workaround for Qualcomm Falkor CPU erratum E1041, where disabling
the MMU without an ISB beforehand can cause the TLB to be populated with
wrong entries. The specific sequence is documented in Qualcomm's errata list.

The `set_sctlr_el1` macro (for enabling) does not need this workaround because:
1. The MMU enable path starts with a clean TLB (from `__cpu_setup`'s `tlbi`)
2. The enable direction is less sensitive than the disable direction for
   this particular erratum

---

## 6. Why NOT Use `msr_s` or Alternative Sequences

Some kernel code uses `msr_s` (a macro that handles system register encodings
not natively understood by all assemblers). For `sctlr_el1`, the standard
`msr` opcode is well-defined and supported by all toolchains. `msr_s` is not
needed here.

---

## 7. The Sequence in Context — Full `__enable_mmu` Annotated

```asm
SYM_FUNC_START(__enable_mmu)
    //
    // Step 1: Validate granule support
    //
    mrs     x3, ID_AA64MMFR0_EL1          // Read memory model features
    ubfx    x3, x3, #TGRAN_SHIFT, 4       // Extract granule field
    cmp     x3, #TGRAN_SUPPORTED_MIN       // Too small?
    b.lt    __no_granule_support           // Park CPU
    cmp     x3, #TGRAN_SUPPORTED_MAX       // Too large?
    b.gt    __no_granule_support           // Park CPU

    //
    // Step 2: Load TTBR0 (identity map)
    //
    phys_to_ttbr x2, x2                   // Encode PA for 52-bit if needed
    msr     ttbr0_el1, x2                 // Write identity map root to TTBR0

    //
    // Step 3: Load TTBR1 (kernel page table root)
    //
    load_ttbr1 x1, x1, x3                // Write kernel page table root to TTBR1
                                          // (includes ISB for workaround)

    //
    // Step 4: Enable the MMU (the critical section)
    //
    set_sctlr_el1 x0                      // DSB SY → MSR SCTLR_EL1 → ISB
    //
    // ← MMU is NOW ACTIVE from this point
    //

    ret                                   // Return via identity map (TTBR0)
SYM_FUNC_END(__enable_mmu)
```

---

## 8. What If the DSB or ISB Were Omitted?

| Omission | Hardware Behavior | Observed Symptom |
|---|---|---|
| No `DSB SY` before MSR | TTBR writes not guaranteed complete; hardware may use old TTBRs | Random translation faults; rare/intermittent data corruption |
| No `ISB` after MSR | Speculatively-fetched post-MMU instructions may have been fetched with wrong state | PC jumps to wrong PA; typically instant crash in `ret` |
| Both omitted | Both hazards combined | Guaranteed crash or data corruption |

This is why ARM64 kernel developers treat `set_sctlr_el1` as sacred — no
"optimization" by removing barriers is acceptable here.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
SCTLR_EL1 is the master hardware control register for the EL1 memory system in ARMv8-A. It is a 64-bit system register written via MSR and read via MRS. On cold reset all bits are zero: MMU off, caches off, alignment checks off. The three most critical bits for the boot path are:
- Bit 0 (M): MMU enable. When set, every VA is translated through TTBR0_EL1 or TTBR1_EL1.
- Bit 2 (C): Data/unified cache enable. L1 D-cache becomes active.
- Bit 12 (I): Instruction cache enable. L1 I-cache becomes active.
The CPU pipeline treats the SCTLR write followed by an ISB as a full memory-system reconfiguration fence: all subsequent instruction fetches and data accesses use the new settings.

### Kernel Perspective (Linux ARM64)
Linux pre-computes the full SCTLR value as the compile-time constant INIT_SCTLR_EL1_MMU_ON in arch/arm64/include/asm/sysreg.h. The value is prepared in __cpu_setup (arch/arm64/mm/proc.S) and passed in x0 to __primary_switch. The kernel never modifies SCTLR bit-by-bit; the full value is written once by set_sctlr_el1 to avoid any intermediate inconsistent state. After start_kernel the register is stable for the lifetime of the CPU.

### Memory Perspective (ARMv8 Memory Model)
With SCTLR_EL1.M=0 the CPU uses a flat physical address space: every load/store address IS the physical address. With M=1 the CPU performs a two-level VA->PA lookup via the page-table walker using TTBR0_EL1 (low VA, user/identity map) and TTBR1_EL1 (high VA, kernel). The transition is instantaneous at the instruction boundary after the ISB that follows the MSR write. The identity-map page tables (mapped at __idmap_text_start) guarantee PA==VA for the code executing at the switch point so the pipeline does not fault.