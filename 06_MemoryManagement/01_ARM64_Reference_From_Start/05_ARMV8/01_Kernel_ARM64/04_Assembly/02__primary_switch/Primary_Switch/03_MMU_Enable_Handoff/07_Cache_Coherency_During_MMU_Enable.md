# Cache Coherency During MMU Enable

**Context:** Cache state transitions when `SCTLR_EL1.C` and `SCTLR_EL1.I` are set  
**Problem:** Page tables were written with D-cache off. Are they visible to the hardware PTW?

---

## 0. The Core Question

Between `__cpu_setup` and `__enable_mmu`, the code executes `__pi_early_map_kernel`
(a C function) which creates page table entries in memory. These writes happen with:

- `SCTLR_EL1.C = 0` — D-cache allocation disabled
- `SCTLR_EL1.M = 0` — MMU is off

When `__enable_mmu` enables the MMU and D-cache simultaneously:

1. Are the page table entries written by C code visible to the hardware PTW?
2. Is there a coherency window where PTW reads stale cache data?
3. Does the kernel need explicit cache maintenance before enabling the MMU?

---

## 1. D-Cache Behavior with `SCTLR_EL1.C = 0`

### What `C = 0` Actually Means

`SCTLR_EL1.C = 0` does NOT mean the cache is fully disabled. The specification says:

> **Stage 1 data cacheability override.** When this field is 0, all data accesses to
> Normal memory from EL1&0 translation regime are treated as Normal Non-Cacheable.

The key word is **override**. The cache hardware is still present and functional.
However:
- Memory accesses are treated as **Non-Cacheable** regardless of MAIR attributes
- Loads bypass the cache and go to the memory system (L2 or DRAM)
- Stores write to the memory system and **do not allocate cache lines**

This is subtly different from "cache is off":
- Existing valid cache lines may still be read (cache coherency via the inner
  shareable domain still applies)
- But new allocations do not happen

### Implication for Page Table Writes

The C code in `__pi_early_map_kernel` writes page table entries:

```c
// Simplified — actual code uses set_pte_at / __set_pmd etc.
pmd[idx] = (pa & GENMASK_ULL(51,21)) | PMD_TYPE_SECT | ... ;
```

With `C = 0`, this store:
1. Writes to the memory system directly (no L1 D-cache allocation)
2. The data is in DRAM (or L2/L3 which are fully coherent)
3. When the hardware PTW reads the same address, it reads from the same
   coherent memory view

**No stale cache problem.** The page table data was never cached, so there
is no stale cache entry to worry about.

---

## 2. Why `C = 0` Was Chosen

**Performance trade-off at boot:**

With `C = 0`:
- Pro: Page table writes go directly to memory → hardware PTW reads fresh data immediately
- Con: All instruction + data accesses are slow (cache-bypassing)

With `C = 1` (what about enabling cache before MMU?):
- Pro: Faster code execution
- Con: Must issue explicit `dc civac` (clean+invalidate) for every page table
  entry written, to ensure coherency with the hardware PTW

The kernel chooses `C = 0` during early boot to avoid per-entry cache maintenance.
After `start_kernel`, the kernel runs with `C = 1` and all normal caching.

---

## 3. The Inner Shareable Domain and Point of Coherency (PoC)

ARM64 defines a memory hierarchy:

```
                    ┌─────────────┐     ┌─────────────┐
CPU 0:              │ L1 I-Cache  │     │ L1 D-Cache  │
                    └──────┬──────┘     └──────┬──────┘
                           │                   │
                    ┌──────┴───────────────────┴──────┐
                    │          L2 Unified Cache        │  ← Inner cache
                    └──────────────────┬───────────────┘
                                       │
               ────────────────────────┼──────────────────── Inner Shareable Domain
                    ┌──────────────────┴───────────────┐
                    │          L3 Shared Cache          │  ← Outer cache
                    └──────────────────┬───────────────┘
                                       │
                    ┌──────────────────┴───────────────┐
                    │             Main Memory (DRAM)    │
                    └──────────────────────────────────┘
```

**Point of Unification (PoU):** Deepest level where I-cache and D-cache are
unified — typically L2. Operations for instruction cache coherency (e.g., after
JIT compilation) only need to reach PoU.

**Point of Coherency (PoC):** The main memory (DRAM). Operations that need to be
visible to all observers (DMA, hardware page table walkers) must reach PoC.

**Hardware PTW coherency level:** The page table walker reads from PoC. This means
that page table entries must be written to main memory (or a cache level at/above
PoC) to be visible to the PTW.

With `C = 0`, all writes go directly to the memory system — they reach PoC
without needing explicit maintenance. This guarantees PTW visibility.

---

## 4. The I-Cache Situation

### State Before MMU Enable

With `SCTLR_EL1.I = 0` (I-cache disabled):
- Instruction fetches bypass the I-cache
- The I-cache may contain stale data from previous boots

On ARM64, when `I = 0`, the I-cache is UNPREDICTABLE. Some implementations
invalidate the I-cache when I is cleared; others leave it in whatever state it
was in.

### After MMU + I-Cache Enable

The `ISB` following `msr sctlr_el1` causes the CPU to:
1. Discard all speculatively-fetched instructions in the pipeline
2. Re-fetch instructions from the correct VAs (using the now-active MMU + identity map)
3. Populate the I-cache with the newly fetched instructions

The ARM Architecture Reference Manual states that after enabling the I-cache
via SCTLR_EL1.I and executing an ISB, software can rely on correct instruction
execution. This implies that any stale I-cache entries are not a problem —
either the hardware invalidates the I-cache on re-enable, or the ISB causes
a pipeline flush that bypasses the stale entries.

---

## 5. Does the Kernel Need `dc civac` Before Enabling the MMU?

**Short answer: No.**

The DSB in `set_sctlr_el1` is a **data synchronization barrier**, not a cache
maintenance operation. It does NOT flush D-cache lines to memory.

But the kernel does NOT need `dc civac` because:
1. Page tables were written with `C = 0` → they are already in memory (not cached)
2. The hardware PTW reads from PoC (main memory), which has the correct data
3. No cache flush is needed because there is no D-cache data to flush for
   those addresses

**Exception — Stack:** Stack writes during C code execution also bypass the cache
(`C = 0`). Stack values (local variables in `__pi_early_map_kernel`) are in
memory. This is functionally correct, just slower than cached stack access.

---

## 6. Cache Coherency for the Identity Map Section

The identity map code (`.idmap.text`) is in the kernel binary, loaded by the
bootloader. The bootloader guarantees that this code is at a known physical
address and is in memory when the kernel starts.

However, **if the bootloader used the I-cache** to execute setup code in the
same physical address range, the I-cache might contain stale entries for
addresses now used by `.idmap.text`.

This is why `__cpu_setup` performs:

```asm
ic      iallu           // I-cache invalidate all to PoU
dsb     nsh             // Ensure invalidation is complete
isb                     // Re-fetch with clean I-cache
```

This happens **before** `__primary_switch` is called. By the time `__enable_mmu`
runs, the I-cache has been invalidated and any stale bootloader instructions
are gone.

---

## 7. The D-Cache Enable Sequence (for completeness)

At the moment `SCTLR_EL1.C` is set to 1 in `__enable_mmu`:
1. The cache hardware remains in the same state (lines already present)
2. The **Non-Cacheable override is lifted** — subsequent accesses may now allocate
   cache lines based on MAIR attributes
3. The D-cache does NOT perform a bulk invalidation; existing cache state (if any)
   may be valid or invalid depending on earlier operations

Since the D-cache was not populated (C=0 during early boot), there are no stale
lines. The cache starts from a clean state. As the kernel executes
`__primary_switched` and eventually `start_kernel`, normal caching begins.

---

## 8. Cache Coherency Timeline Summary

| Phase | D-cache State | I-cache State | Page Table Coherency |
|---|---|---|---|
| Bootloader | Unknown | May be active | N/A |
| `primary_entry` | Not in use (C=0) | Invalidated in `__cpu_setup` | N/A |
| `__cpu_setup` | C=0 (Non-Cacheable) | I=0, invalidated | N/A |
| `__pi_early_map_kernel` | C=0, writes go to memory | I=0 | Tables in memory ✓ |
| `set_sctlr_el1` DSB | C=0 | I=0 | DSB ensures memory ops complete |
| `set_sctlr_el1` MSR | **C=1 I=1 M=1 enabled** | enabled | PTW reads from memory |
| `set_sctlr_el1` ISB | C=1, start allocating | I=1, re-fetch | TLB fill begins |
| `__primary_switched` | Fully cached | Fully cached | Normal operation |

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