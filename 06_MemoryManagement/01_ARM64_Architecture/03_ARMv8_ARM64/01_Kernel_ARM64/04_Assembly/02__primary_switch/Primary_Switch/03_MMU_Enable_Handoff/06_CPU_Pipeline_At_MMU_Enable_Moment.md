# CPU Pipeline State at the MMU Enable Moment

**Context:** What happens inside the CPU pipeline when `msr sctlr_el1, x0` commits  
**Architecture:** ARMv8.2-A out-of-order superscalar pipeline (reference: Cortex-A76 TRM)

---

## 0. Why This Matters

The instruction `msr sctlr_el1, x0` (which sets `SCTLR_EL1.M = 1`) is the most
consequential single instruction in ARM64 boot. Understanding the micro-
architectural effects is required to reason about:
- Why barriers are mandatory
- What the identity map continuity requirement really protects
- Why the kernel cannot safely reach here with the MMU already on

---

## 1. A 12-Stage Out-of-Order Pipeline Overview

```
                     ┌──────────────────────────────────────────────────────┐
Front End            │  IFetch → IBuf → Decode → Rename/Dispatch           │
                     └──────────────────────────────────────────────────────┘
                     ┌──────────────────────────────────────────────────────┐
Out-of-Order Engine  │  Issue Queue → Execute (ALU/LSU/Branch/SVE/FP)      │
                     └──────────────────────────────────────────────────────┘
                     ┌──────────────────────────────────────────────────────┐
Retirement           │  ROB → Commit/Retire → Architectural State Update   │
                     └──────────────────────────────────────────────────────┘
```

Key principle: Instructions are **fetched** in-order, **executed** out-of-order,
and **committed** in-order via the Reorder Buffer (ROB).

System register writes (`msr sctlr_el1`) are **serializing operations** — they
drain all prior instructions from the ROB before committing and do not allow
younger instructions to pass them in execution.

---

## 2. The `msr sctlr_el1` Instruction — Stage by Stage

### Stage 1: Instruction Fetch (IF)

The fetch unit retrieves the `msr sctlr_el1, x0` instruction bytes from:
- **Before the ISB in `load_ttbr1`:** PA (identity-mapped, MMU off)
- **The fetch itself:** Uses physical address directly (MMU off)

The fetch unit prefetches several subsequent instructions too:
- `ret` (the instruction after `set_sctlr_el1`)
- The caller's return sequence in `__primary_switch`

These pre-fetched instructions are in the instruction buffer.

### Stage 2: Decode and Rename

The `msr sctlr_el1, x0` is decoded as a system register write. The register
renaming stage:
- Reads the physical register mapped to architectural register `x0`
- Tags the instruction with ROB entry #N

### Stage 3: Issue and Execute

System register writes (MSR instructions) are handled by the **System Unit**
in the execution engine. The system unit:
1. Waits until all prior instructions have left the ROB (serializing behavior)
2. Writes the value to the SCTLR_EL1 shadow register
3. Marks the ROB entry complete

**Shadow register:** SCTLR_EL1 is typically implemented as two registers:
- `SCTLR_EL1_architectural` — the value software reads back with `mrs`
- `SCTLR_EL1_live` — the value the hardware actually uses for address translation

The live register is updated only at the ISB (commit point), not at the MSR.

### Stage 4: ROB Retirement

The `msr sctlr_el1, x0` instruction retires from the ROB. This is the
**architectural commit point** — from here, the MMU state has changed.

### Stage 5: ISB Execution

The `isb` instruction (next in sequence) is a pipeline flush operation:
1. All instructions in the fetch queue and pipeline stages ahead of the ISB
   are **squashed** (discarded without effect)
2. The `SCTLR_EL1_live` register is updated to match `SCTLR_EL1_architectural`
3. The instruction fetch unit is redirected to the instruction following the ISB
4. The program counter (PC) for the re-fetch is computed as:
   - If MMU is now on: VA = instruction address after ISB → translated by MMU
   - The MMU uses TTBR0 (identity map) for the VA of the `.idmap.text` section

---

## 3. The Fetch Boundary: PA → VA Transition

This is the most critical microarchitectural moment. After the ISB:

```
Before ISB:  CPU fetches using PA directly
After ISB:   CPU fetches using VA → translated by MMU via TTBR0 identity map
```

```
PC value at fetch:   0x4020_1000  (physical PA, example)

MMU looks up 0x4020_1000 via TTBR0:
    → __pi_init_idmap_pg_dir maps VA 0x4020_1000 → PA 0x4020_1000
    → Translation succeeds (identity map)
    → Instruction fetched from PA 0x4020_1000
    → Same bytes as before!

Result: Execution continues without interruption.
```

This is the **identity map continuity guarantee**: The VA equals the PA for
`.idmap.text`, so the virtual-to-physical translation is the identity function,
and the CPU continues executing the same bytes.

---

## 4. What Happens to Pre-Fetched Instructions

The ISB squashes all instructions that were speculatively fetched before the
ISB executed. Specifically:

```
Instruction queue before ISB commits:
    [dsb sy]           ← already retired
    [msr sctlr_el1]    ← retiring now
    [isb]              ← current
    [ret]              ← IN QUEUE, will be SQUASHED by ISB
    [br x8]            ← IN QUEUE (from caller), will be SQUASHED
```

After ISB:
```
    [ret]              ← RE-FETCHED using new SCTLR (MMU active, VA via TTBR0)
    [br x8]            ← RE-FETCHED using new SCTLR
```

The `ret` instruction in `__enable_mmu` pops the link register and branches.
Its target address `x30` (link register) = VA of the instruction after `bl __enable_mmu`
in `__primary_switch`. This VA is in the identity-mapped `.idmap.text` section,
so the fetch succeeds.

---

## 5. Speculative TLB Population

A concern: Can the hardware PTW speculatively populate TLB entries for
instructions fetched between the `msr sctlr_el1` (MMU enabled) and the `isb`?

**ARM Architecture position:** The ARM ARM states that the architectural effect
of `msr sctlr_el1` is defined as "completed" at the ISB. Before the ISB, the
CPU is in an UNPREDICTABLE state regarding whether address translation uses the
old or new SCTLR value.

**Practical implication:** A CPU implementation may choose to enable the MMU as
soon as the `msr sctlr_el1` retires, or may wait until the ISB. The ISB is
required to make the behavior architecturally defined.

**Why this doesn't matter:** Even if the CPU enables the MMU speculatively and
translates addresses between the `msr` and `isb`, the identity map ensures that
those translations give correct results (VA = PA for `.idmap.text`).

---

## 6. Cache State at MMU Enable

### Before MMU Enable

- `SCTLR_EL1.C = 0` → D-cache disabled (L1 D-cache acts as RAM without evictions)
- `SCTLR_EL1.I = 0` → I-cache disabled (instruction fetches bypass cache)

But `__cpu_setup` sets `SCTLR_EL1.I = 1` and `SCTLR_EL1.C = 1` in the value
written to `x0`, which is then passed to `set_sctlr_el1`. So the `msr` that
enables the MMU **simultaneously** enables both caches.

### After MMU Enable

- L1 D-cache: Now active. Stores from `__pi_early_map_kernel` (page table writes)
  that were cached in the L1/L2 while D-cache was off... wait.

Actually, with `SCTLR_EL1.C = 0`, the D-cache does NOT cache — it operates in
write-through mode to main memory, and reads bypass the cache. So page table
data written by C code (`__pi_create_init_idmap`) during the pre-MMU phase
went to physical memory. When the MMU is enabled and the D-cache is enabled,
the next reads of the page table data (by the hardware PTW) will use the memory
values, and the result is cached in the D-cache.

There is no stale cache issue here because C=0 means no D-cache was populated.

### I-Cache and the ISB

When the I-cache is enabled (as part of the same SCTLR write), the `isb` that
follows causes the instruction pipeline to be flushed. The re-fetched instructions
come from the **physical** address via the I-cache (now enabled). The I-cache
will fetch the `.idmap.text` section bytes and cache them.

This is why `ISB` is sufficient — no explicit `ic iallu` (I-cache invalidate
all) is needed at this point because:
1. The I-cache was disabled before and is now empty (or contains UNPREDICTABLE
   state if the hardware implementation pre-populates it)
2. On ARM64, enabling the I-cache with a fresh ISB is defined to give correct behavior
   (any stale I-cache entries are treated as invalid on the first enable)

---

## 7. Branch Predictor State

At MMU enable, the branch predictor (BTB, BHT, indirect branch predictor) may
contain entries from:
1. Previous boots (warm boot / kexec)
2. Speculative execution during the current boot

These entries map VAs to target VAs (or PAs on some implementations). After
the MMU is enabled, VAs are now virtual, but BP entries from the pre-MMU phase
are indexed by PA-as-VA (the physical addresses that were used as VAs before
MMU enable).

For the `ret` in `__enable_mmu` and the `br x8` in `__primary_switch`, the
branch predictor:
- Either misses (returns to recovery path, computes target from link register)
- Or has a stale entry that redirects to wrong address → misprediction detected
  at commit → pipeline flushed → recovery path takes over

A branch predictor misprediction is recovered safely. It causes a 10-20 cycle
penalty but does not affect correctness.

**Spectre v2 concern:** Malicious Spectre branch target injection would require
an attacker to pre-poison the BTB before boot. In the boot context this is not
a threat (there is no attacker code running). The `nospectre_v2` kernel command
line option affects whether IBRS/IBPB mitigations are applied post-boot.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The MMU in ARMv8-A is enabled by writing bit 0 (M) of SCTLR_EL1 to 1 via an MSR instruction followed by an ISB. The ISB is the critical barrier: it flushes the instruction pipeline so that all instructions fetched AFTER the ISB use the new memory system configuration. Before the MMU is enabled, the CPU operates in a flat physical address space. After the bit is set, the TLB, page-table walker, TTBR0/TTBR1, TCR_EL1, and MAIR_EL1 all become active simultaneously. There is no intermediate state.

### Kernel Perspective (Linux ARM64)
Linux enables the MMU in __enable_mmu (arch/arm64/kernel/head.S), called from __primary_switch. The sequence is:
  1. Write TTBR0_EL1 (identity map root).
  2. Write TTBR1_EL1 (kernel map root).
  3. ISB to synchronize TTBR writes.
  4. Write SCTLR_EL1 with M=1 (via set_sctlr_el1 macro).
  5. ISB to flush the pipeline.
  6. RET -- the very next instruction is fetched through the new MMU.
The identity map ensures that the physical address of the code after the RET is also mapped at the same VA (PA==VA), so no instruction-fetch fault occurs.

### Memory Perspective (ARMv8 Memory Model)
The moment SCTLR_EL1.M is written to 1 and the ISB completes, the ARMv8 memory model transitions from "flat PA" to "two-stage VA->PA via page tables". The identity map (stored in __idmap_text_start to __idmap_text_end, mapped in the .idmap.text section) covers the physical pages of the MMU-enable code so the VA==PA invariant holds during the critical window. Without the identity map, the instruction fetch for the RET after set_sctlr_el1 would target a VA that has no valid TLB entry, causing a translation fault with no exception handler installed yet.