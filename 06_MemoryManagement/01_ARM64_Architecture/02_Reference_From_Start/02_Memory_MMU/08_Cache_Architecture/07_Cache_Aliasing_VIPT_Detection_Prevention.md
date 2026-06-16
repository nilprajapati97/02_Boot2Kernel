# Cache Aliasing: Detection, Prevention, and Handling

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Cache Aliasing Definition

```
Cache aliasing: two different virtual addresses (VA1, VA2) map to the SAME
physical address (PA), but use DIFFERENT cache sets (different cache line slots).

Result: the same physical memory can have TWO cache entries:
  Cache set X (indexed by VA1): contains value "A" (stale, from earlier write)
  Cache set Y (indexed by VA2): contains value "B" (newer write via VA2)
  
  CPU reading via VA1: returns "A" (stale)
  CPU reading via VA2: returns "B" (current)
  → Two processes see DIFFERENT values of the same physical memory!
  → Data corruption, security violation (one process reads another's data)

When does VA1 and VA2 map to same PA?
  1. Shared memory: two processes mmap() the same file at different VAs
  2. Kernel + user aliasing: kernel maps a page at kernel VA; 
     user process maps same page at user VA
  3. KPTI: trampoline page mapped at both kernel and user VAs
  4. DMA: driver maps a DMA buffer at multiple VAs
  5. ioremap + phys_to_virt: same PA accessed via different kernel VAs

Different cache set = aliasing:
  For VIPT cache with N sets of L bytes per line:
  Set index = VA[log2(N×L)-1 : log2(L)]
  If VA1 and VA2 have different bits in this range → different sets → aliasing!
```

---

## 2. ARM64 VIPT Aliasing Analysis

```
ARM64 VIPT L1 D-cache (typical):
  64 sets, 4-way, 64B lines
  Index bits: VA[11:6] (log2(64 × 64) = log2(4096) = 12 bits total, offset=6)
  Page offset bits: VA[11:0]
  
  For 4KB pages: PA = PTBASE | VA[11:0]
  VA[11:0] is IDENTICAL to PA[11:0] (page offset is same in VA and PA)
  
  Since index = VA[11:6] ⊆ VA[11:0] = PA[11:0]:
  → VA[11:6] ≡ PA[11:6] (index bits are within page offset)
  → Two VAs mapping same PA have same VA[11:0] → same index!
  → NO aliasing possible with 4KB pages

ARM64 VIPT aliasing with LARGE pages:
  2MB pages: VA[20:12] determines which 4KB sub-page within the 2MB page
  PA = LARGEPTBASE | VA[20:0]  (21-bit page offset for 2MB)
  PA[11:0] = VA[11:0] still holds
  VA[11:6] still same as PA[11:6] → still no aliasing with 2MB pages

What if L1 D-cache were LARGER (hypothetical):
  64KB 4-way VIPT: 256 sets, 64B lines
  Index bits: VA[13:6]
  Bits [13:12] are NOT within page offset bits [11:0]
  PA[13:12] ≠ VA[13:12] (different virtual pages, same physical page)
  → VA1 could have VA[13:12]=0b00, VA2 could have VA[13:12]=0b01
  → VA1 index ≠ VA2 index → different cache sets → ALIASING!
  
  ARM prevents this by: per-way size ≤ page size
  64KB 4-way: per-way = 16KB > 4KB page → WOULD alias
  ARM64 cores limit L1 D-cache to: ≤ page_size × associativity
  
  Cortex-A78: 32KB, 4-way: per-way = 8KB > 4KB → ALIASING POSSIBLE!
  Wait — ARM claims A78 L1 D-cache is 32KB, 4-way...
  
  Resolution: ARM A78 uses a virtual-physical hybrid (checks both tags)
  OR: the cache is designed with hash-based indexing that uses PHYSICAL bits
  AND: Linux's page coloring constraints prevent conflicting mappings
  In practice: modern ARM64 cores' index range stays within [11:0] regardless
  of nominal cache size (they use advanced indexing to avoid aliasing)
```

---

## 3. Page Coloring

```
Page coloring: software technique to prevent cache aliasing
  
OS assigns "colors" to physical pages based on which cache sets they can occupy
  A page's "color" = PA[cache_index_bits] (the bits used for cache set selection)
  
For VIPT with index bits [13:6] (hypothetical 64KB 4-way L1):
  Color = PA[13:12] (bits 12 and 13 of physical address)
  4 possible colors: 0b00, 0b01, 0b10, 0b11
  
Rule: Two VAs that might alias MUST map to physical pages of the SAME color
  (same cache index bits from PA → same cache set → no aliasing)
  
  For shared memory (file-backed):
    All mappings of the same page offset in a file must use the same-colored PA
    OS ensures: when mapping page at VA X, allocate PA with color = VA[color_bits]
    
Linux historically:
  Alpha, MIPS, PA-RISC, older SPARC: implemented page coloring in buddy allocator
  ARM64: avoids page coloring by design (per-way ≤ page size)
  x86: PIPT L1 D-cache → no aliasing → no page coloring needed

Flush-on-alias (alternative to page coloring):
  Instead of page coloring: flush cache when mapping a page at a new VA
  Before mapping PA at new VA:
    DC CIVAC for all cache lines of PA (using old VA if known, or set/way flush)
  This ensures: old VA's cache lines are flushed before new VA can alias
  Expensive: flush on every new mapping of shared pages
  Used by: older ARM32 Linux (VIPT with aliasing, without page coloring)
```

---

## 4. Shared Memory Aliasing Case Study

```
Scenario: two processes share a file (mmap):
  Process A: mmap(file, offset=0) → VA = 0x40000000
  Process B: mmap(file, offset=0) → VA = 0x50000000
  
  Both VAs map to the same physical page PA=0x12340000
  
  With 64-byte cache lines, 64-set VIPT L1 D-cache (index = VA[11:6]):
    VA=0x40000000: VA[11:6] = 0b000000 (same as PA[11:6] since bits in page offset)
    VA=0x50000000: VA[11:6] = 0b000000 (same, since 0x50000000[11:6] = 0b000000)
    Both map to SAME cache set → no aliasing (this works fine on ARM64)
  
  With hypothetical aliasing cache (index = VA[13:6]):
    VA=0x40000000: VA[13:6] = 0x000 >> 6 = 0b00_000000 → set 0
    VA=0x50000000: VA[13:6] = bits 13-6 = 0x50_000000 >> 6 [13:6]
      0x50000000 = 0101_0000_0000_..._0000
      bits [13:6] = 0b00000000 = set 0 (still same)
    
  The aliasing risk only occurs when VA[13:12] differ for two mappings of same PA
  On ARM64 standard 4KB page, VA[11:0] = PA[11:0] always → safe
  
Linux shared memory note:
  shm_open, mmap(MAP_SHARED): all CPUs see consistent data via cache coherency
  (CPUs ARE coherent with each other via MESI/ACE protocol)
  Cache aliasing is distinct from cache coherency:
    Coherency: same VA, different cores → hardware handles this
    Aliasing: different VAs → same PA → hardware CANNOT detect (VAs differ)
```

---

## 5. Instruction Cache Aliasing

```
I-cache aliasing (VIPT or VIVT I-cache):
  Many ARM64 cores have VIPT I-cache
  Same concern: two VAs for same code page → different I-cache sets
  
  ARM64 L1 I-cache: typically VIPT or PIPT
  If VIPT: same constraints as D-cache (per-way ≤ page size)
  
  However: I-cache is READ-ONLY (code doesn't write to I-cache)
  → Aliasing for I-cache means: two mappings of same code page
    may have separate I-cache entries, but both will have correct data
    (both were loaded from the same PA via PTW)
  → No correctness issue (both copies are read-only and identical)
  → Possible PERFORMANCE issue (I-cache occupancy doubled) but no bug
  
  Exception: self-modifying code / JIT:
    JIT writes new code at VA1 (D-cache at VA1 updated)
    Code executed at VA2 (different VA, same PA)
    I-cache at VA2 may have OLD code (from before JIT wrote VA1)
    
    Fix: flush_icache_range(VA2, end) after JIT writes via VA1
    (DC CVAU at VA2 + IC IVAU at VA2 — not VA1!)
    Because: IC IVAU uses the execution VA (VA2), not the write VA (VA1)

CTR_EL0.L1Ip[15:14] = I-cache policy:
  0b00 = VPIPT (Physically Indexed Physically Tagged — no aliasing)
  0b01 = AIVIVT (Legacy, not used in ARM64)
  0b10 = VIPT (Virtually Indexed Physically Tagged)
  0b11 = PIPT (Physically Indexed Physically Tagged — no aliasing)
  
  Most ARM64 cores: CTR_EL0.L1Ip = 0b11 (PIPT I-cache) or 0b10 (VIPT, safe)
  Linux checks this: if L1Ip = PIPT, skip I-cache flush for aliases
```

---

## 6. Interview Questions & Answers

**Q1: ARM64 uses VIPT L1 D-cache. Under what conditions can aliasing occur, and how does ARM64 prevent it?**

VIPT (Virtually Indexed, Physically Tagged) uses virtual address bits to select the cache set. Cache aliasing occurs when two VAs map to the same PA but compute DIFFERENT cache set indices. For a 4KB page size, the page offset occupies VA[11:0], and since the page offset is identical in VA and PA (both use the same lower 12 bits), any cache index bits within [11:0] will be the same for both VAs mapping the same PA.

ARM64 prevents aliasing by ensuring the **per-way cache size ≤ page size (4KB)**. The set index for a VIPT cache is determined by `VA[log2(sets×line_size)-1 : log2(line_size)]`. If `sets × line_size = per-way size ≤ 4KB`, then the entire index range falls within [11:0] — identical for all VAs mapping the same PA. For example, a 16KB 4-way L1 D-cache: per-way = 4KB, sets × 64B = 4096B = 4KB → index bits are [11:6] ⊆ [11:0] → no aliasing.

Modern ARM Cortex-A cores technically have larger L1 D-caches (32KB, 64KB), but they use advanced hardware indexing (hashing, cross-way index bits) that maps the full cache while keeping the effective VIPT index within [11:0]. Linux doesn't need page coloring for ARM64 because this hardware design guarantee holds.

---

## 7. Quick Reference

| Cache Size | Ways | Per-Way | Index Range | Aliasing with 4KB pages? |
|---|---|---|---|---|
| 16KB | 4-way | 4KB | [11:6] | No (within page offset) |
| 32KB | 4-way | 8KB | [12:6] | Possible (bit12 differs) |
| 32KB | 8-way | 4KB | [11:6] | No (within page offset) |
| 64KB | 4-way | 16KB | [13:6] | Possible (bits 12,13 differ) |
| 64KB | 16-way | 4KB | [11:6] | No (within page offset) |

| Memory sharing scenario | Aliasing risk | Fix |
|---|---|---|
| mmap of same file at same VA | None (same VA) | N/A |
| mmap of same file at different VAs | Possible with large VIPT | Page coloring / flush |
| JIT: write at VA1, execute at VA2 | Yes (I-cache stale) | IC IVAU at VA2 |
| DMA buffer: kernel + user VA | Possible | Use VIPT-safe mapping |
