# ARM64 Cache Architecture: Complete Interview Reference

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Master Cache Architecture Summary

```
ARM64 Cache System Overview:

  CPU Core
  ├── L1 I-cache: 32–64KB, VIPT, 4-way, per-core (4 cycles)
  ├── L1 D-cache: 32–64KB, VIPT, 4-way, per-core (4–6 cycles)
  │     Both Harvard (separate I+D paths)
  │     
  ├── L2 Unified: 256KB–8MB, PIPT, 8–16 way, per-core (10–20 cycles)
  │     PoU: Point of Unification (I+D become coherent here)
  │     Uses: DC CVAU + IC IVAU for code patching
  │     
  └── L3/LLC: 4–64MB, PIPT, shared across cores (30–50 cycles)
        PoC: Point of Coherency (all agents see same data)
        CCI/CCN interconnect maintains coherency here
        Uses: DC CIVAC/CVAC for DMA operations
        
  DRAM: 40–100 ns (100–300 cycles), all PA accessible

Key identifiers:
  CTR_EL0: cache line sizes (DminLine, IminLine), IDC, DIC bits
  CLIDR_EL1: cache level types, LoC, LoUIS, LoU
  CSSELR_EL1 + CCSIDR_EL1: cache geometry (sets, ways, line size)
```

---

## 2. All Cache Maintenance Instructions

```
Instruction Reference:
  DC CVAU, Xn   → Clean D-cache VA to PoU    [JIT/code patch step 1]
  DC CVAC, Xn   → Clean D-cache VA to PoC    [DMA_TO_DEVICE]
  DC CIVAC, Xn  → Clean+Inv VA to PoC        [DMA_BIDIRECTIONAL]
  DC IVAC, Xn   → Invalidate VA at PoC       [DMA_FROM_DEVICE, careful!]
  DC ZVA, Xn    → Zero cache line            [clear_page(), fast memset]
  DC CISW, x0   → Clean+Inv by Set/Way       [BOOT ONLY, not SMP-safe!]
  IC IVAU, Xn   → Inv I-cache VA to PoU      [JIT/code patch step 2]
  IC IALLU      → Inv all I-cache to PoU     [per-CPU full I-cache flush]
  IC IALLUIS    → Inv all I-cache IS PoU     [broadcast to all IS CPUs]
  
Mandatory post-operation barriers:
  After DC CVAU: DSB ISH
  After IC IVAU: DSB ISH + ISB
  After DC CIVAC (for DMA): DSB ISH
  After DC IVAC (for DMA): DSB ISH
  All ISB required when: about to execute code (after IC ops) or switch contexts

Shortcuts (CTR_EL0 optimization):
  CTR_EL0.IDC = 1: Skip DC CVAU before IC IVAU
  CTR_EL0.DIC = 1: Skip IC IVAU after DC CVAU
```

---

## 3. Critical Design Rules

```
Rule 1: DMA direction = correct cache maintenance direction
  DMA_TO_DEVICE:     DC CVAC  (clean dirty CPU data to DRAM → device reads it)
  DMA_FROM_DEVICE:   DC IVAC  (discard stale CPU copy → CPU reads fresh DRAM)
  DMA_BIDIRECTIONAL: DC CIVAC (both: clean then discard)
  Wrong direction → data corruption or data loss

Rule 2: Never DC IVAC dirty cache lines unless you know they're clean
  DC IVAC on dirty data: discards the dirty data without writing back
  → Silent data loss (no error, data just gone)
  When in doubt: use DC CIVAC (clean+invalidate = always safe)

Rule 3: VIPT aliasing prevention: per-way size ≤ page size (4KB)
  64 sets × 64B = 4KB per way: SAFE for VIPT
  256 sets × 64B = 16KB per way: DANGEROUS (bits [13:12] differ in VA/PA aliases)
  ARM64 cores: never exceed 4KB per way for VIPT D-cache

Rule 4: JIT code flush must use execution VA (not write VA)
  JIT writes new code at VA1 (possibly different from execution VA VA2)
  IC IVAU must use VA2 (the address the CPU will FETCH from)
  DC CVAU uses VA1 (the address the CPU WROTE to)

Rule 5: Set/Way flush (DC CISW) is NOT SMP-safe
  DC CISW only flushes the LOCAL CPU's cache
  Other CPUs may still have dirty copies
  Always use VA-based operations (DC CIVAC) for SMP cache management

Rule 6: Cache maintenance requires DSB before dependent accesses
  DC CIVAC; STR x0, [x1]   ← WRONG: STR may start before CIVAC completes!
  DC CIVAC; DSB ISH; STR x0, [x1]  ← CORRECT: DSB ensures serialization
```

---

## 4. Common Interview Questions with Answers

**Q1: Describe the complete sequence for a JIT compiler to make generated code executable on ARM64.**

```
1. Write machine code to a buffer via D-cache (normal store instructions)
2. DC CVAU for each cache line in [buf_start, buf_end)  → push to PoU (L2)
3. DSB ISH  → wait for all DC CVAU to complete
4. IC IVAU for each cache line in [buf_start, buf_end)  → invalidate L1 I-cache
5. DSB ISH  → wait for all IC IVAU to complete
6. ISB      → flush instruction pipeline
7. Now safe to jump to buf_start and execute

If CTR_EL0.IDC=1: skip step 2 (D-cache automatically coherent with L2 at PoU)
If CTR_EL0.DIC=1: skip step 4 (I-cache automatically sync'd)

Linux: flush_icache_range(buf_start, buf_end) handles steps 2–6
```

**Q2: Why does a DMA FROM_DEVICE transfer sometimes corrupt data in neighboring struct fields?**

The DMA buffer is not cache-line aligned (or the struct field is not at a cache-line boundary). When DC IVAC is performed for `DMA_FROM_DEVICE`, it invalidates the **entire cache line** (64 bytes). If the buffer starts or ends in the middle of a cache line, the neighboring fields (other struct members before or after the buffer) that share that cache line are also invalidated. After the invalidation, the CPU reads those neighboring fields from DRAM — but DRAM may have stale data if the CPU had previously written those fields (dirty cache data for the neighbors was discarded by DC IVAC). The fix: always align DMA buffers to cache-line boundaries and ensure DMA buffers don't share cache lines with non-DMA data.

**Q3: Explain false sharing and its performance impact on a 16-core ARM64 system.**

False sharing occurs when two CPU cores write to different variables that happen to reside in the same 64-byte cache line. Example: a `struct { long count_cpu0; long count_cpu1; }` where both fields are in the same cache line. Core 0 writes `count_cpu0` → that cache line is in M (Modified) state on Core 0. Core 1 writes `count_cpu1` → the ARM64 coherency protocol (ACE/CCI) detects Core 0 has the line in M state → sends Invalidate to Core 0 → Core 0 writes back and invalidates. Core 1 gets the cache line, writes `count_cpu1` → now Core 1 has it in M state. Core 0 writes again → repeat. On 16 cores writing concurrently, every write triggers an invalidation broadcast to all 15 other cores. Each write takes ~150 cycles (cache coherency round-trip) instead of ~1 cycle (local cache hit). The fix: `__cacheline_aligned` padding, or `DEFINE_PER_CPU` variables.

---

## 5. Cache Type Quick Reference

| Attribute | VIPT L1 D-cache | PIPT L2 | PIPT L3 |
|---|---|---|---|
| Index | Virtual | Physical | Physical |
| Tag | Physical | Physical | Physical |
| Aliasing | Possible (if per-way>4KB) | No | No |
| ARM64 L1 safe? | Yes (per-way ≤ 4KB) | N/A | N/A |
| Latency | 4–6 cycles | 10–20 cycles | 30–50 cycles |
| Shared? | Per-core | Per-core | Shared |

## 6. Memory Attribute Selection Reference

| Use Case | MAIR Attribute | SH Bits | PTE Flags |
|---|---|---|---|
| Kernel data/code | Normal WB-WA (0xFF) | Inner Shareable (0b11) | PAGE_KERNEL |
| User pages | Normal WB-WA (0xFF) | Inner Shareable (0b11) | PROT_NORMAL |
| DMA coherent buffer | Normal NC (0x44) | Inner Shareable (0b11) | PROT_NORMAL_NC |
| MMIO registers | Device nGnRE (0x04) | Non-Shareable (0b00) | PROT_DEVICE_nGnRE |
| PCIe config space | Device nGnRnE (0x00) | Non-Shareable (0b00) | PROT_DEVICE_nGnRnE |
| Write-combining FB | Device nGnRE or WT | Non-Shareable | pgprot_writecombine() |

## 7. Cortex CPU Cache Sizes Reference

| CPU | L1 I | L1 D | L2 | L3 | Process |
|---|---|---|---|---|---|
| Cortex-A55 | 32KB | 32KB | 128–256KB | — | 7nm+ |
| Cortex-A77 | 64KB | 64KB | 256KB–1MB | — | 7nm |
| Cortex-A78 | 32KB | 32KB | 512KB–4MB | — | 5nm |
| Cortex-X2 | 64KB | 64KB | 1MB | — | 4nm |
| Neoverse N2 | 64KB | 64KB | 2MB | 32–64MB | 5nm |
| Apple M2 (P) | 192KB | 128KB | 16MB | — | 5nm |
