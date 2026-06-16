# Memory Compaction and Fragmentation Remediation

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

External fragmentation occurs when free memory is available but not in contiguous blocks of the required order.

Compaction migrates movable pages to coalesce free space. Incorrectly tuned, compaction introduces its own latency spikes and TLB churn.

---

## 2. ARM64 Hardware Detail

### 2.1 Compaction on ARM64 large-page workloads

ARM64 platforms heavily using THP (2 MB) or hugetlb (1 GB) need physically contiguous allocations. Fragmentation directly causes THP fallback to base pages, losing the TLB coverage benefit.

### 2.2 Compaction-induced TLB traffic

Page migration during compaction unmaps and remaps pages, generating TLB invalidation broadcasts. On large ARM64 SoCs with many cores, this can temporarily inflate IPI load.

---

## 3. Linux Kernel Implementation

Key components:
- `kcompactd` — background compaction daemon per NUMA node
- `vm.compaction_proactiveness` (0–100) — how aggressively kcompactd targets fragmentation
- `vm.extfrag_threshold` — trigger threshold based on fragmentation index
- `/proc/buddyinfo` — free-list state per order, diagnoses fragmentation levels
- `/sys/kernel/debug/extfrag/extfrag_index` — per-zone fragmentation index

### 3.1 Diagnosing fragmentation

1. check `/proc/buddyinfo`: are high-order (9+) free lists depleted?
2. read `extfrag_index`: values near 1.0 indicate severe fragmentation
3. correlate with THP allocation failures in vmstat (`thp_fault_fallback`)
4. review `compact_stall` counter for compaction blocking allocators

### 3.2 Tuning levers

| Knob | Direction | Effect |
|---|---|---|
| `vm.compaction_proactiveness` | raise to 20–40 | earlier background compaction |
| THP policy | set to `madvise` | reduce THP demand pressure |
| `vm.min_free_kbytes` | raise slightly | maintain high-order free reserves |
| CMA reservation at boot | increase | guarantee large contiguous allocations |

### 3.3 Safe recovery sequence

1. observe fragmentation and compaction stall counters
2. raise `compaction_proactiveness` incrementally
3. monitor TLB shootdown side effects
4. revert if latency worsens despite lower fragmentation

---

## 4. Hardware-Software Interaction

The interaction is a balance:
- ARM64 THP and hugetlb need high-order pages → compaction needed
- compaction causes migration → TLB shootdowns
- more cores mean wider IPI broadcasts per migration

Proactive compaction during low-load windows is far cheaper than emergency compaction during peak traffic.

---

## 5. Interview Q and A

Q1: What does high `compact_stall` mean?  
Allocators are blocking on synchronous compaction — a direct latency source.

Q2: Why does fragmentation hurt ARM64 more than x86 in some workloads?  
ARM64 systems often rely more on THP for TLB efficiency; fallback to base pages degrades performance more sharply.

Q3: What should you watch for when raising `compaction_proactiveness`?  
TLB shootdown overhead and any p99 latency regressions from increased migration activity.

Q4: How does CMA help in embedded ARM64 designs?  
It reserves physically contiguous regions at boot, avoiding runtime compaction for DMA and GPU allocations.

Q5: When is defrag the wrong remedy?  
When the real problem is workload overcommit, not fragmentation — fix sizing first.

Q6: Best compaction timing strategy?  
Run proactively during predictable low-traffic windows rather than reactively during peaks.

---

## 6. Pitfalls and Gotchas

- High `compaction_proactiveness` on low-memory embedded systems can continuously churn pages.
- Assuming fragmentation is always the cause of high-order allocation failures without checking memory watermarks.
- Ignoring `thp_fault_fallback` as a fragmentation signal.
- Compacting without considering NUMA node locality of migrated pages.

---

## 7. Quick Reference Table

| Signal | Interpretation | Response |
|---|---|---|
| high-order buddy lists empty | fragmentation | raise compaction proactiveness |
| `compact_stall` growing | sync compaction in allocators | pre-empt with proactive kcompactd |
| `thp_fault_fallback` rising | THP can't get 2MB pages | defragment or switch to `madvise` |
| p99 spikes during compaction | migration overhead | tune proactiveness down, spread work |
