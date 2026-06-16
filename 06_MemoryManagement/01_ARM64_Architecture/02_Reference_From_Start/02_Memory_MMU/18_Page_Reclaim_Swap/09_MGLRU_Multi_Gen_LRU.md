# MGLRU (Multi-Gen LRU) Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

MGLRU improves reclaim quality by tracking pages in multiple generations instead of only active/inactive lists.

Core idea:
- older generations are colder
- reclaim prefers oldest generation first
- reduces wrong evictions of hot pages

Result:
- lower reclaim overhead
- better tail latency under pressure

---

## 2. ARM64 Hardware Detail

### 2.1 Access tracking signal

ARM64 page table entries expose AF (Access Flag), which Linux uses as one signal for page aging.

### 2.2 TLB interaction

Aging scans and PTE checks can trigger TLB maintenance costs if done aggressively.
MGLRU aims to reduce unnecessary churn.

---

## 3. Linux Kernel Implementation

### 3.1 Generation model

Each lruvec maintains multiple generations:
- youngest generation: recently used pages
- oldest generation: cold candidates

### 3.2 Aging and eviction

Flow:
1. aging pass promotes accessed pages to younger generation
2. reclaim pass targets oldest generation
3. pages with references are spared
4. unreferenced pages are evicted or swapped

### 3.3 Memcg and NUMA awareness

MGLRU integrates with memcg and lruvec per-node accounting.
Each node/cgroup can have independent pressure behavior.

---

## 4. Hardware-Software Interaction

Pressure timeline:
1. workload spikes memory usage
2. kswapd starts reclaim
3. MGLRU selects oldest generation first
4. fewer hot-page evictions compared to classic LRU
5. swap I/O and refaults reduce

---

## 5. Interview Q and A

Q1: Why does MGLRU outperform classic active/inactive LRU?
Because generation-based history approximates working-set age more accurately.

Q2: Does MGLRU replace all reclaim logic?
No. It changes page selection strategy; reclaim infrastructure remains similar.

Q3: How does MGLRU affect swap activity?
Usually lowers unnecessary swap-outs of recently hot pages.

Q4: Is MGLRU beneficial for server workloads?
Yes, especially mixed workloads with large memory footprints.

Q5: What is the main cost of MGLRU?
Aging bookkeeping overhead, usually much smaller than reclaim miss penalties.

Q6: How to validate MGLRU gain?
Compare refault rate, PSI, and tail latency under pressure.

---

## 6. Pitfalls and Gotchas

- Assuming MGLRU removes all reclaim stalls.
- Ignoring memcg-specific behavior while tuning globally.
- Over-interpreting one benchmark without mixed workload tests.
- Not correlating refault drops with actual latency gain.

---

## 7. Quick Reference Table

| Feature | Description |
|---|---|
| Multi-generation aging | keeps better history than binary active/inactive |
| Oldest-first reclaim | targets colder pages first |
| Refault reduction | fewer hot-page evictions |
| Memcg/NUMA integration | per-lruvec pressure handling |
