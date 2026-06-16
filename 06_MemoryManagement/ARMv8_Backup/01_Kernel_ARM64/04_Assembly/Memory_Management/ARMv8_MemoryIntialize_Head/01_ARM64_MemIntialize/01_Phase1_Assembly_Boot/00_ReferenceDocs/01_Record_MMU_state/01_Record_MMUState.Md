## `record_mmu_state` — Detailed ARMv8 Explanation

This function detects the MMU/cache state at boot and records it in `x19` for later use. It also fixes endianness mismatches early.

---

### Phase 1: Determine Exception Level (lines 135–138)

```asm
mrs   x19, CurrentEL       // read CurrentEL system register
cmp   x19, #CurrentEL_EL2  // compare against EL2 (value 0x8)
mrs   x19, sctlr_el1       // speculatively read SCTLR_EL1
b.ne  0f                    // if not EL2, skip ahead (keep sctlr_el1)
mrs   x19, sctlr_el2       // we ARE at EL2, override with SCTLR_EL2
```

- **`CurrentEL`** (bits [3:2]): encodes the current exception level. EL1 = `0x4`, EL2 = `0x8`.
- **`SCTLR_ELx`** (System Control Register): controls MMU enable, caches, endianness, etc. The code reads the SCTLR for *whichever EL we're running at*, since that's the one governing current execution.

After this block, **`x19` = the active SCTLR** and the **condition flags still reflect the EL2 comparison** (`Z=1` if EL2, `Z=0` if EL1). This is critical — `b.ne` later reuses these flags.

---

### Phase 2: Check Endianness (lines 141–142)

```asm
CPU_LE( tbnz  x19, #SCTLR_ELx_EE_SHIFT, 1f )
CPU_BE( tbz   x19, #SCTLR_ELx_EE_SHIFT, 1f )
```

- **`SCTLR_ELx.EE`** (bit 25): Exception Endianness. `0` = little-endian, `1` = big-endian.
- `CPU_LE()` / `CPU_BE()` are compile-time macros — only one emits code depending on the kernel's configured endianness.
- **If kernel is LE**: branch to `1f` if EE=1 (hardware is BE, mismatch → needs fixing).
- **If kernel is BE**: branch to `1f` if EE=0 (hardware is LE, mismatch → needs fixing).

If endianness **matches**, fall through to Phase 3. If **mismatched**, jump to Phase 4.

---

### Phase 3: Record MMU + Cache State (lines 143–146)

```asm
tst   x19, #SCTLR_ELx_C    // test C bit (data cache enable), sets Z if C==0
and   x19, x19, #SCTLR_ELx_M  // isolate M bit (MMU enable)
csel  x19, xzr, x19, eq    // if Z==1 (caches off): x19 = 0; else: x19 = M bit
ret
```

- **`SCTLR_ELx.M`** (bit 0): MMU enable. `1` = MMU on.
- **`SCTLR_ELx.C`** (bit 2): Data cache enable. `1` = D-cache on.
- The logic encodes three states into `x19`:

| MMU (M) | D-Cache (C) | Result in x19 | Meaning                                    |
|---------|-------------|---------------|--------------------------------------------|
| 0       | 0           | `0`           | MMU off (normal boot)                      |
| 1       | 0           | `0`           | MMU on but caches off — treated as MMU-off |
| 1       | 1           | `1` (M bit)   |  MMU and caches on (e.g., EFI boot)        |
| 0       | 1           | `0`           | Impossible in practice                     |

The caller (`primary_entry`) uses `x19 != 0` to decide whether cache maintenance uses **invalidate** (MMU was off) or **clean** (MMU was on).

---

### Phase 4: Fix Endianness Mismatch (lines 154–165)

```asm
1:  eor   x19, x19, #SCTLR_ELx_EE  // toggle EE bit to match kernel endianness
    bic   x19, x19, #SCTLR_ELx_M   // clear M bit (disable MMU)
    b.ne  2f                         // branch uses flags from the EL2 cmp above!
    pre_disable_mmu_workaround       // Cortex-A errata workaround
    msr   sctlr_el2, x19            // we're at EL2: write corrected SCTLR_EL2
    b     3f
2:  pre_disable_mmu_workaround
    msr   sctlr_el1, x19            // we're at EL1: write corrected SCTLR_EL1
3:  isb                              // instruction synchronization barrier
    mov   x19, xzr                   // x19 = 0 (MMU is now off)
    ret
```

Key points:

- **`EOR` toggles EE**: flips the endianness to match what the kernel expects.
- **`BIC` clears M**: the MMU **must** be disabled when changing endianness, because the page tables were built for the original byte order — interpreting them with flipped endianness would cause immediate faults.
- **`b.ne 2f`**: the condition flags are *still* from the `cmp x19, #CurrentEL_EL2` at line 136. If we were at EL1 (NE), write `sctlr_el1`; if EL2 (EQ), write `sctlr_el2`.
- **`ISB`**: required after writing SCTLR to ensure the pipeline sees the new endianness/MMU settings before any subsequent instruction fetch.
- **`x19 = 0`**: since we just disabled the MMU, report MMU-off state to the caller.

---

### Summary

The function answers two questions for the boot path:
1. **Was the MMU+cache on at entry?** → stored in `x19` (nonzero = yes)
2. **Is the CPU endianness wrong?** → if so, fix it immediately by toggling `SCTLR.EE`, disable the MMU, and report MMU-off


When a LE kernel finds `EE=1` (CPU is in big-endian mode), execution branches to label `1:` (line 154) and does the following:

### Step-by-step at label `1:`

```asm
1:  eor   x19, x19, #SCTLR_ELx_EE   // (1) toggle EE: 1→0 (switch to LE)
    bic   x19, x19, #SCTLR_ELx_M     // (2) clear M bit (force MMU off)
    b.ne  2f                           // (3) if EL1, go to 2f
    msr   sctlr_el2, x19              // (4a) EL2: write fixed SCTLR
    b     3f
2:  msr   sctlr_el1, x19              // (4b) EL1: write fixed SCTLR
3:  isb                                // (5) synchronize pipeline
    mov   x19, xzr                     // (6) report MMU-off
    ret
```

### What each step accomplishes

1. **`EOR` flips EE from 1→0**: This changes the CPU's data endianness from big-endian to little-endian, matching what the LE kernel expects. Without this, every memory load/store (including instruction fetches from the ID map) would interpret bytes in the wrong order.

2. **`BIC` clears M (MMU off)**: The page tables were populated by the bootloader or EFI stub using the *old* byte order. If the MMU stayed on after flipping endianness, the translation table walker would read page table entries with reversed byte order → immediate translation fault / garbage mappings. So the MMU **must** be disabled first.

3. **`B.NE` selects the correct EL register**: The condition flags still reflect the `cmp x19, #CurrentEL_EL2` from line 136. The modified SCTLR is written to whichever exception level the CPU is actually running at (`sctlr_el2` or `sctlr_el1`).

4. **`MSR` writes the corrected SCTLR**: The hardware immediately begins using the new configuration after the next synchronization point.

5. **`ISB` flushes the pipeline**: This is mandatory — the ARM architecture requires an ISB after writing SCTLR so that all subsequent instructions are fetched and decoded under the new endianness and MMU settings. Without it, the CPU could execute stale instructions fetched under BE ordering.

6. **`x19 = 0`**: Since the MMU was just disabled, the function reports "MMU off" to the caller. This tells `primary_entry` to use **cache invalidation** (not clean) for the page tables it builds next, because with the MMU off, any cached data is stale/irrelevant.

### Why this matters

If the kernel *didn't* do this, every `ldr`/`str` instruction would swap bytes. For example, loading the 32-bit value `0x12345678` from memory would yield `0x78563412`. The kernel's compiled code, data structures, and page tables all assume LE byte order, so execution would crash almost immediately — likely on the very next memory access after `record_mmu_state` returns.
