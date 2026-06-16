# Cache Maintenance — Invalidate vs Clean

**Source:** `arch/arm64/kernel/head.S` lines 96–117

## The Problem

ARM64 CPUs have a **weak memory model**. Caches and RAM can hold different data for the same address. Before any MMU state transition (off→on or on→off), you must ensure they agree — or the CPU will read garbage.

## The Two Paths

After `__pi_create_init_idmap` returns, `primary_entry` forks based on `x19`:

```asm
cbnz  x19, 0f          ; if x19 != 0 (MMU was on), go to label 0
; --- MMU-OFF path ---
dmb   sy
mov   x1, x0           ; x1 = end of page table region (returned by create_init_idmap)
adrp  x0, __pi_init_idmap_pg_dir  ; x0 = start of page table region
adr_l x2, dcache_inval_poc
blr   x2               ; invalidate cache for page table region
b     1f

; --- MMU-ON path ---
0:  adrp  x0, __idmap_text_start
    adr_l x1, __idmap_text_end
    adr_l x2, dcache_clean_poc
    blr   x2            ; clean cache for .idmap.text
```

## Path A: MMU was OFF — Invalidate

**Scenario:** Normal boot (e.g., U-Boot). MMU and D-cache are off.

**Problem:** Even with D-cache disabled, ARM64 CPUs can **speculatively** load cache lines. While `create_init_idmap` was writing page table entries directly to RAM, the cache may have loaded stale data for those same addresses (whatever garbage was in memory before).

**Solution:** `dcache_inval_poc` — **Invalidate** (discard) all cache lines covering the page table region. This ensures when the MMU is later enabled, the CPU will fetch the correct page table entries from RAM.

```
Before invalidation:
  Cache:  [stale data from before kernel loaded]
  RAM:    [correct page table entries]

After invalidation:
  Cache:  [empty — lines discarded]
  RAM:    [correct page table entries]  ← CPU reads from here
```

**`dmb sy`** — Data Memory Barrier. Ensures all previous stores (the page table writes) are visible to all observers before the invalidation starts. Without this, the invalidation might race with the stores.

## Path B: MMU was ON — Clean

**Scenario:** EFI boot. MMU and D-cache were enabled by firmware.

**Problem:** The page table writes went through the cache (cache was on). Now `init_kernel_el` is about to **disable the MMU**. After that, the CPU will fetch instructions from RAM directly. But the `.idmap.text` code (which includes `__enable_mmu` and related functions) might still be sitting in the cache, not yet written back to RAM.

**Solution:** `dcache_clean_poc` — **Clean** (write back) all cache lines for the `.idmap.text` section to the Point of Coherency. This ensures RAM has the latest data before the MMU is turned off.

```
Before cleaning:
  Cache:  [latest .idmap.text instructions — dirty]
  RAM:    [possibly stale copy]

After cleaning:
  Cache:  [clean — matches RAM]
  RAM:    [latest .idmap.text instructions]  ← CPU fetches from here after MMU off
```

**Note:** This path cleans `.idmap.text` (the code region), not the page tables. The page tables were also written through cache, but they'll be used after MMU is re-enabled (when cache is coherent again).

## What is Point of Coherency (PoC)?

PoC is the point in the memory hierarchy where all observers (all CPU cores, DMA, GPU) see the same data. Typically this is main memory (DRAM).

```
CPU Core 0          CPU Core 1
    │                   │
    ▼                   ▼
 L1 Cache            L1 Cache
    │                   │
    ▼                   ▼
 L2 Cache            L2 Cache
    │                   │
    └───────┬───────────┘
            ▼
     Last-Level Cache (L3)
            │
            ▼
    ═══════════════════
     DRAM (Point of Coherency)
    ═══════════════════
```

`dcache_clean_poc` pushes data all the way to DRAM.
`dcache_inval_poc` discards all cached copies down to DRAM.

## Summary

| Condition | Action | Target | Why |
|-----------|--------|--------|-----|
| `x19 == 0` (MMU off) | **Invalidate** | Page table region | Discard speculative stale cache lines |
| `x19 != 0` (MMU on) | **Clean** | `.idmap.text` code | Flush instructions to RAM before MMU-off |

## Key Takeaway

Cache maintenance is **not optional**. Skipping it causes silent corruption: the CPU reads old data and either executes wrong instructions or walks corrupt page tables. The two-path design handles both normal boot (MMU off) and EFI boot (MMU on) correctly.
