# 99.02 — Cache Interview Question Bank

> Curated cache questions for 10-year ARM-systems interviews.

---

## Hierarchy & Coherency

**Q1. Describe a typical ARM cache hierarchy.**

L1: split I-cache (VIPT) + D-cache (PIPT), 32–64 KB each, per-core. L2: unified, per-core or per-cluster, 256 KB–1 MB. L3 / SLC: shared at cluster or chip level, 4–32 MB, in the CMN interconnect. Coherency maintained over CHI/ACE (UC/UD/SC/SD/I states). See [05.01](../05_Caches/01_Cache_Hierarchy_L1_L2_L3.md).

---

**Q2. PoU vs PoC vs PoP?**

**PoU (Point of Unification)**: where I and D views converge — typically the L2 cache. Sufficient for self-modifying code on the same PE. **PoC (Point of Coherency)**: where all observers (including non-coherent agents) see a single value — typically DRAM or SLC. Needed for DMA. **PoP (Point of Persistence)** (FEAT_DPB): for persistent-memory writeback. See [05.02](../05_Caches/02_PoU_PoC_Inner_Outer.md).

---

**Q3. Difference between MESI and MOESI?**

MOESI adds **O (Owned)** — lets a dirty line be **shared** with other caches without writeback to memory. The owner serves cache-to-cache transfers; eventual writeback on eviction. Saves memory bandwidth on read-heavy shared dirty data. See [05.04](../05_Caches/04_Cache_Coherency_MESI_MOESI.md).

---

**Q4. What ARM coherency states correspond to MESI?**

ARM CHI states: UC (Unique Clean) = E, UD (Unique Dirty) = M, SC (Shared Clean) = S, SD (Shared Dirty) = O, I = I.

---

**Q5. What's a Read-For-Ownership?**

A read that simultaneously invalidates all other cached copies, preparing for a write. CHI transaction: `ReadUnique`. Avoids two round-trips for read-then-write.

---

## Maintenance Ops

**Q6. Difference between `DC CVAU` and `DC CVAC`?**

`DC CVAU` cleans D-cache line to PoU (sufficient for I-cache coherency on same PE — JIT use case). `DC CVAC` cleans to PoC (needed for non-coherent DMA / cross-observer visibility). See [05.03](../05_Caches/03_Cache_Maintenance_Ops_DC_IC.md).

---

**Q7. Why are set/way operations not used for coherency?**

They're per-PE (not broadcast). Running CPUs may re-allocate or modify lines mid-sequence. Architecturally meant for power-down save/restore, not coherency.

---

**Q8. Self-modifying code sequence on ARM.**

```
1. Write new instructions to memory.
2. DC CVAU per line   (clean D to PoU)
3. DSB ISH            (wait for completion)
4. IC IVAU per line   (invalidate I to PoU, broadcast)
5. DSB ISH
6. ISB                (flush local pipeline)
```

If `CTR_EL0.IDC=1` and `DIC=1` (modern parts), some steps can be skipped.

---

**Q9. DMA outbound sequence (non-coherent device).**

```
DC CVAC over [buf, buf+len)    (clean to PoC)
DSB OSH                        (outer-shareable barrier)
start DMA
```

---

**Q10. DMA inbound sequence.**

Before: `DC IVAC` over the buffer (invalidate so cache doesn't merge stale data with DMA-written data). After: `DSB OSH`, then read.

---

## VIPT / PIPT / Aliasing

**Q11. Why VIPT for I-cache and PIPT for D-cache?**

VIPT: index lookup runs in parallel with TLB translation → lower latency, critical for fetch. PIPT: no aliasing → safe for store-forwarding, coherency. See [05.05](../05_Caches/05_VIPT_PIPT_Aliasing.md).

---

**Q12. What's the size constraint for a VIPT cache without aliasing?**

`cache_size / associativity ≤ page_size`. With 4 KB pages: direct-mapped 4 KB, or 4-way 16 KB. Larger requires page coloring or HW alias detection.

---

**Q13. Synonyms vs Homonyms?**

Synonym: many VAs → one PA (problem in VIVT/VIPT). Homonym: one VA → many PAs across contexts (problem in VIVT only — PIPT/VIPT tag-physical avoid it).

---

**Q14. What's page coloring?**

OS restricts VA→PA assignment so the cache-index bits of VA equal those of PA — eliminates synonyms for VIPT caches above the constraint size.

---

## Performance

**Q15. Categories of cache misses?**

The 4 C's plus coherency: Compulsory, Capacity, Conflict, Coherency. Plus prefetch-pollution evictions.

---

**Q16. How would you detect false sharing in production?**

`perf c2c` — Linux tool using PMU events to find cache lines bouncing between CPUs. Mitigate with cache-line padding and per-CPU data structures. See [05.06](../05_Caches/06_Cache_Performance_Prefetch.md).

---

**Q17. When is `PRFM` useful?**

When HW prefetchers can't detect the pattern (indirect, scatter/gather, irregular strides) AND PMU shows memory-stall dominance. Otherwise PRFM often pollutes cache and hurts. Profile first.

---

**Q18. What's MPAM?**

Memory Partitioning And Monitoring — architectural QoS lets software partition cache capacity and memory bandwidth across partition IDs. Cloud/server use case. ARM equivalent of Intel CAT.

---

**Q19. What's the "transient" hint in MAIR?**

A Normal-memory attribute flag indicating short-lived data; replacement policy may bias eviction so longer-lived data is retained. Apple silicon uses this.

---

**Q20. How does NUMA affect cache?**

Remote-socket cache lines have higher fetch latency and snoop costs. First-touch / explicit affinity (`numactl`, `mbind`) keeps working sets local. Critical on Neoverse-based servers and multi-socket Snapdragon/Grace systems.

---

## Cross-refs

- [99.01 MMU questions](01_MMU_Interview_Questions.md)
- [99.03 Barrier questions](03_Barrier_and_Ordering_Questions.md)
- [99.04 System design scenarios](04_System_Design_Scenarios.md)
