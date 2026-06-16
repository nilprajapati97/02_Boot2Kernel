# 04.04 — TLB Performance, Contiguous Bit, Hugepages

> **ARM ARM Reference**: §D5.10

---

## 1. Why TLB Performance Matters

A TLB miss = walk = ≥4 DRAM accesses (more under stage-2). On a workload with poor TLB hit rate, **the MMU dominates execution time**, not the data path. This is why HPC/databases on arm64 obsess about TLB tuning.

---

## 2. Levers to Improve TLB Performance

| Lever | Effect |
|---|---|
| Hugepages (block descriptors) | One TLB entry covers MB/GB |
| Contiguous bit | One TLB entry covers a coalesced group |
| Larger granule (16K, 64K) | Larger base page = larger reach |
| Process layout (compact heap) | Fewer TLB-distinct pages touched |
| NUMA-aware allocation | Avoids cross-node walk costs |
| `mm_cpumask` tracking | Reduces shootdown overhead |
| ASID-tagged TLB | No flush on context switch |
| `nG=0` for kernel | Survives ASID change |

---

## 3. Hugepages — Detail

### Allowed sizes by granule

| Granule | Block at L1 | Block at L2 | Page |
|---|---|---|---|
| 4K  | 1 GB | 2 MB | 4 KB |
| 16K | —    | 32 MB | 16 KB |
| 64K | —    | 512 MB | 64 KB |

### Linux mechanisms
- **Transparent Hugepages (THP)** — automatic 2 MB hugepages for anonymous memory, opportunistically promoted.
- **HugeTLBfs** — explicit reservation of hugepage pools (2 MB / 1 GB).
- **Hugepage-aware allocators** (`jemalloc`/`tcmalloc` with hugepage flags).

### Pitfalls
- 2 MB block at L2 → TLB miss still needs 3 walks (L0 → L1 → L2), but only one TLB entry to cover 2 MB.
- 1 GB block: 2 walks; one TLB entry covers 1 GB → enormous reach.

---

## 4. The Contiguous Bit — Detail

A group of N adjacent identical PTEs marked contiguous can be folded into one TLB entry by the hardware.

| Granule / Level | Group size | Coalesced size |
|---|---|---|
| 4K / L3 | 16 | 64 KB |
| 4K / L2 block | 16 | 32 MB |
| 16K / L3 | 128 | 2 MB |
| 64K / L3 | 32 | 2 MB |

### Constraints
- All N PTEs must have **identical attrs and perms**.
- All N must be naturally aligned (both VA and PA) to N × granule.
- Contiguous bit set on **all N**.
- Changing one requires BBM on all N (architectural requirement).

The contiguous bit is a "free" performance feature when you already have N adjacent identical mappings — Linux uses it on the linear map.

---

## 5. Measuring with PMU

Key Performance Monitor counters:

| Event | Meaning |
|---|---|
| `L1D_TLB_REFILL` | L1 data TLB refills |
| `L1I_TLB_REFILL` | L1 instr TLB refills |
| `L2D_TLB_REFILL` | L2 TLB refills |
| `DTLB_WALK` | Data TLB walks |
| `ITLB_WALK` | Instr TLB walks |
| `STALL_BACKEND_TLB` | Cycles stalled due to TLB |

Use `perf stat -e ...` to attribute cost.

---

## 6. Vendor Notes

- **Apple silicon** ships with 16 KB granule + large TLBs — minimizes walk pressure even without hugepages.
- **Server-class Neoverse** parts have multi-thousand-entry L2 TLBs and aggressive walk caches.
- **GPU SMMUs** rely heavily on hugepages (often 2 MB) to keep IOTLB hit rates high.

---

## 7. Pitfalls

1. **Allocating data 4 KB at a time** when it could be 2 MB-block-backed → 512× TLB pressure.
2. **Mixing attributes within a contiguous candidate group** → can't use contig bit.
3. **THP fragmenting** under memory pressure → khugepaged collapse cost.
4. **Hugepage promotion mid-execution** requires BBM and is expensive — measure before enabling.

---

## 8. Interview Q&A

**Q1. Why are hugepages a TLB optimization?**
One TLB entry covers MB/GB instead of KB → vastly higher reach for the same TLB capacity.

**Q2. Trade-off of hugepages?**
Internal fragmentation; potentially more cost on first allocation/zeroing; less flexible permission/migration granularity.

**Q3. What's the contiguous bit?**
A PTE flag indicating a group of N adjacent identical PTEs can be merged into one TLB entry.

**Q4. Difference between a 2 MB block descriptor and a 16-PTE contig group (4K)?**
Block = single descriptor at L2; contiguous = many descriptors at L3 with hint. Block walks are 3 levels deep; contig group walks are 4 levels deep but each entry coalesces into TLB.

**Q5. How would you diagnose TLB-bound performance?**
PMU: `L2D_TLB_REFILL`, `DTLB_WALK` — high counts vs IPC drop suggests TLB-bound; consider hugepages.

**Q6. Why might 64 KB granule actually hurt a workload?**
Internal fragmentation: small allocations bloated to 64 KB → DRAM/L1 pressure may rise more than TLB savings.

---

## 9. Cross-refs

- [01 TLB architecture](01_TLB_Architecture_and_Tagging.md)
- [03.03 Block vs page](../03_Page_Tables_and_Translation/03_Block_vs_Page_Mappings.md)
- [10.05 PMU counters](../10_Advanced_and_Vendor_Context/05_Performance_Counters_PMU.md)
