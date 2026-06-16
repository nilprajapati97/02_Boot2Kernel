# Huge Pages Complete Reference for ARM64

Category: Huge Pages and Page Size Optimization  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Huge pages optimize memory efficiency and performance by reducing translation overhead and TLB pressure.

Categories:
- transparent huge pages: automatic, opportunistic
- explicit hugetlb: reserved, deterministic
- architecture-specific sizes: 2MB, 1GB, potentially larger with CONT

Core tradeoff:
- huge pages improve throughput and reduce translation cost
- fragmentation, split overhead, and reclaim complexity increase

---

## 2. ARM64 Hardware Detail

### 2.1 Supported mappings

Common ARM64 huge page sizes:
- 2MB (at PMD level)
- 1GB (at PUD level)
- 32MB or 64MB via CONT hint optimization on supporting cores

### 2.2 TLB and translation efficiency

Huge page benefit:
- one TLB entry covers large range
- fewer page-table walks
- better caching behavior in TLB-heavy workloads

### 2.3 Compaction and fragmentation

Physical memory must be sufficiently contiguous.
Compaction mechanics maintain supply of large-order free blocks.

---

## 3. Linux Kernel Implementation Summary

Key subsystems:
- page allocator and buddy system for huge allocation
- THP subsystem for transparent allocation and collapse
- hugetlb subsystem for explicit reservation
- memory compaction for defragmentation
- page reclaim interacting with huge page lifecycle

---

## 4. Hardware-Software Interaction

Workload path:
1. application accesses memory
2. kernel evaluates huge-page eligibility
3. THP or hugetlb allocates if possible
4. large mapping installed
5. TLB efficiency improved for covered range
6. under pressure, may split or reclaim
7. background collapse may restore

Operational observation:
- throughput gains depend on workload stability and size
- latency may spike during splits or compaction
- fragmentation gradually degrades availability

---

## 5. Interview Q and A

Q1: THP or hugetlb for typical server workload?
THP for general-purpose; hugetlb for known huge-memory applications requiring determinism.

Q2: Why does 1GB mapping help more than 2MB on large systems?
Fewer overall entries needed, better TLB efficiency for very large address ranges.

Q3: Is CONT hint transparent to software?
Mostly. Hardware can use it for larger TLB entries, but software doesn't always see the benefit directly.

Q4: What is the biggest risk of aggressive huge pages?
Excessive compaction latency and fragmentation complexity outweighing throughput gains.

Q5: How do you validate huge page deployment?
Measure TLB miss rate, allocation success/fallback ratio, and end-to-end latency under realistic load.

Q6: Why is this category so broad in scope?
Because huge pages affect virtually every memory subsystem: allocation, reclaim, compaction, fragmentation.

---

## 6. Pitfalls and Gotchas

- Enabling THP globally without workload analysis.
- Confusing huge page availability with actual performance improvement.
- Underestimating compaction and split latency overhead.
- Over-tuning based on micro-benchmarks that don't reflect real usage.
- Assuming hugetlb is always better than THP without measuring.

---

## 7. Quick Reference Across Category

| Feature | Type | Typical benefit |
|---|---|---|
| THP at fault | automatic | improved TLB for eligible workloads |
| khugepaged collapse | background | large-page recovery after fragmentation |
| hugetlb pool | explicit | deterministic availability for known huge-memory apps |
| 1GB blocks | architecture | excellent efficiency for contiguous regions |
| CONT hints | optimization | TLB entry size hints to hardware |
| Compaction | defrag | enables large allocation when fragmented |

| Decision point | Typical choice |
|---|---|
| OLTP database | hugetlb with 2MB pre-allocated |
| general compute | THP with madvise hints |
| real-time sensitive | careful tuning or disable THP |
| memory-constrained | THP with defrag=defer to avoid stalls |
