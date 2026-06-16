# 99.03 — Barriers and Memory-Ordering Interview Questions

> Curated barrier / consistency questions.

---

## Foundations

**Q1. What is ARMv8's memory model?**

Weakly ordered, other-multi-copy-atomic. Most reorderings allowed unless dependencies or barriers prevent. All observers agree on the global order of writes to different locations once causally ordered (added in v8; v7 was not multi-copy atomic). See [01.04](../01_Memory_Model/04_Weakly_Ordered_Memory_Model.md), [06.04](../06_Memory_Barriers_Ordering/04_Coherency_vs_Consistency.md).

---

**Q2. Difference between coherency and consistency.**

Coherency: per-address, what value do you read for X. Consistency: cross-address, what ordering of writes across X, Y, Z do you observe. Orthogonal: ARM is fully coherent yet weakly consistent. See [06.04](../06_Memory_Barriers_Ordering/04_Coherency_vs_Consistency.md).

---

**Q3. What's "multi-copy atomic"?**

Property where all observers agree on the global order of writes to **different locations**. ARMv8 provides "other-multi-copy atomic" — a PE may see its own store before others (store-buffer forwarding), but other PEs see all stores in a consistent global order.

---

## DMB / DSB / ISB

**Q4. Difference between DMB, DSB, ISB?**

**DMB**: orders memory accesses without waiting for completion. **DSB**: blocks until all preceding memory + maintenance ops complete. **ISB**: flushes pipeline; subsequent fetches see preceding context-changing ops (SCTLR, TTBR, etc.). See [06.01](../06_Memory_Barriers_Ordering/01_DMB_DSB_ISB.md).

---

**Q5. What shareability scope for SMP within a socket?**

`ISH` (Inner Shareable). Tighter than SY (full system) yet covers all coherent CPUs in the cluster. Linux `smp_*` barriers compile to ISH variants.

---

**Q6. Cheapest barrier for store-store ordering across SMP?**

`DMB ISHST` — Inner Shareable, store-store only. Maps to Linux `smp_wmb()`.

---

**Q7. Why is ISB needed after enabling the MMU?**

SCTLR.M write is a Context-Changing Operation; without ISB, decoded instructions in the pipeline may already be present from before the change and may execute under stale context.

---

**Q8. TLBI sequence — why DSB ISH; ISB?**

`TLBI ...IS` is broadcast but asynchronous. `DSB ISH` waits until all PEs ack. `ISB` then flushes the local pipeline so subsequent fetches use the new mapping. DMB after TLBI is wrong — TLBI is a maintenance op requiring DSB.

---

## Acquire / Release

**Q9. Difference between LDAR and LDAPR?**

LDAR is **RCsc** (Release Consistency, sequentially consistent) — strong; STLR↔LDAR pair is sequentially consistent globally. LDAPR (ARMv8.3, FEAT_LRCPC) is **RCpc** (processor-consistent) — weaker; later accesses ordered after locally, but cross-thread STLR–LDAPR sequence not guaranteed sequentially consistent. See [06.02](../06_Memory_Barriers_Ordering/02_Acquire_Release_LDAR_STLR.md).

---

**Q10. Why doesn't STLR need a trailing barrier?**

Release semantics only require earlier-accesses-before-this-store; later accesses are unconstrained. That asymmetry is the optimization.

---

**Q11. What does Linux `smp_store_release` / `smp_load_acquire` compile to?**

`STLR` / `LDAR` respectively on arm64.

---

**Q12. C11 `memory_order_seq_cst` maps to what on arm64?**

`LDAR` for loads, `STLR` for stores. RCsc semantics give SC behavior. Some patterns additionally need a `DMB` (typically for SB-like cross-thread store-then-load patterns).

---

**Q13. LSE atomics (FEAT_LSE) — why?**

Replace LDXR/STXR exclusive-monitor loops with single-instruction atomics: `LDADD`, `CAS`, `SWP` (plus A/L/AL acquire/release variants). Scale better under contention because they avoid reservation-loss livelock; interconnect can serialize them efficiently. Critical on many-core Neoverse servers.

---

## Litmus

**Q14. MP (Message Passing) — is `r1=1, r2=0` allowed?**

Without barriers: ALLOWED on ARM (store reorder on producer, load reorder on consumer). Fix: `STLR flag` on producer + `LDAR flag` on consumer; or DMB pairs. See [06.03](../06_Memory_Barriers_Ordering/03_Load_Store_Reordering_Examples.md).

---

**Q15. SB (Store Buffer) — is `r1=0, r2=0` allowed?**

ALLOWED on ARM without barriers — each thread's load can pass its own store via the store buffer. Fix: `DMB ISH` between store and subsequent load on each thread. LDAR/STLR alone is **insufficient** here.

---

**Q16. IRIW on ARMv8 — allowed?**

FORBIDDEN — multi-copy atomicity guarantees all observers see writes to different locations in the same global order. (Allowed on pre-v8 ARM.)

---

**Q17. LB (Load Buffering) — `r1=1, r2=1` allowed?**

FORBIDDEN. Would require values from thin air; ARM forbids speculation that produces such outcomes.

---

**Q18. CoRR — coherency of reads to same address.**

`r1=1, r2=0` for two successive reads is FORBIDDEN universally — basic single-location coherency.

---

## Dependencies

**Q19. What ordering does an address dependency provide?**

Load → dependent load (via address) is ordered automatically — no barrier needed. RCU's `rcu_dereference` exploits this on arm64 (no smp_rmb needed).

---

**Q20. Why doesn't a control dependency from a load order subsequent loads?**

Branch prediction may speculatively execute the subsequent loads before the dependency-load resolves. ISB or DMB needed if order matters. (Control dependency *to a store* is ordered, because stores can't be speculative.)

---

## Cross-refs

- [99.01 MMU questions](01_MMU_Interview_Questions.md)
- [99.02 Cache questions](02_Cache_Interview_Questions.md)
- [99.04 System design scenarios](04_System_Design_Scenarios.md)
