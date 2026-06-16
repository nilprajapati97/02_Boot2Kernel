# Huge Page and Memory Fragmentation Deep Dive

Category: Huge Pages and Page Size Optimization  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Huge page success depends critically on availability of contiguous physical memory.

Fragmentation problem:
- over time, free pages become scattered across address space
- large allocation requests fail even when total free pages exceed requirement
- impacts both THP and hugetlb reliability

Mitigation strategies:
- memory compaction
- CMA reservations
- allocation policies favoring contiguous regions

---

## 2. ARM64 Hardware Detail

### 2.1 Fragmentation at allocator level

Buddy allocator organizes free pages by order:
- order-N: 2^N contiguous pages
- large allocations require high-order free pages
- fragmentation occurs when high-order pages are unavailable

### 2.2 Compaction mechanics

Memory compaction reorganizes pages:
- scans and moves movable pages toward boundary
- creates free regions of required contiguity
- driven by allocation pressure or background policy

### 2.3 CMA and allocation segregation

Contiguous Memory Allocator reserves pools:
- ensures large-allocation regions remain defragmented
- reduces fragmentation by dedicating space policy-wise
- kernel allocations outside CMA, devices may use CMA

---

## 3. Linux Kernel Implementation

### 3.1 Compaction path

When order-9 (2MB) allocation fails:
1. allocator tries compaction
2. compaction scans and migrates pages
3. retry allocation after compaction
4. may fail again if fragmentation is severe

### 3.2 Compaction triggers

Automatic compaction is invoked by:
- direct reclaim during high-order allocation
- background compaction thread (if enabled)
- explicit request from fragmentation-aware code

### 3.3 Defrag policy controls

User controls influence behavior:
- /sys/kernel/debug/kcompactd_max_order
- /sys/kernel/mm/page_migrate_demote_zones
- madvise MADV_HUGEPAGE hints

### 3.4 CMA configuration

CMA can be reserved at boot:
- via kernel parameter cma=
- device-tree definitions
- platform-specific default reservations

---

## 4. Hardware-Software Interaction

Allocation flow under fragmentation:
1. high-order allocation request arrives
2. buddy allocator checks free lists
3. required order unavailable
4. initiates compaction or direct reclaim
5. compaction moves pages to reduce fragmentation
6. retry allocation
7. may still fail if fundamental shortage exists

Result:
- THP gets huge pages when alignment permits
- hugetlb may stall or fail depending on defrag and CMA policy

---

## 5. Interview Q and A

Q1: What is the relationship between buddy order and huge pages?
Buddy allocates in power-of-2 chunks; order-9 is required for 2MB (512 × 4KB pages).

Q2: Why is fragmentation harder to fix than allocation failures?
Because it requires moving occupied memory and is inherently latency-impactful.

Q3: What is CMA and how does it help fragmentation?
CMA reserves memory regions that stay mostly defragmented by excluding normal allocations.

Q4: When should compaction be aggressive versus lazy?
Aggressive for latency-sensitive workloads willing to tolerate stalls; lazy for throughput-focused systems.

Q5: How do you measure effective fragmentation?
By tracking high-order allocation success/failure ratios and compaction event frequencies.

Q6: What policy reduces THP fallback most?
Keeping CMA reserved and background compaction enabled to maintain high-order availability.

---

## 6. Pitfalls and Gotchas

- Assuming unlimited CMA reservation; it reduces general allocator pool.
- Underestimating latency of heavy compaction operations.
- Tuning compaction without measuring workload phase changes.
- Not accounting for compaction failing if memory is truly full.
- Forgetting that some page types are not migratable and cannot be compacted.

---

## 7. Quick Reference Table

| Technique | Benefit |
|---|---|
| Buddy order logic | naturally groups contiguous pages |
| Memory compaction | moves pages to create high-order free regions |
| CMA reservation | dedicates defragmented space for large allocations |
| Background kcompactd | proactive fragmentation management |

| Observable | Meaning |
|---|---|
| high thp_fault_fallback rate | fragmentation preventing THP |
| compaction_stall counter | direct reclaim compacted |
| CMA allocation failures | insufficient CMA space |
