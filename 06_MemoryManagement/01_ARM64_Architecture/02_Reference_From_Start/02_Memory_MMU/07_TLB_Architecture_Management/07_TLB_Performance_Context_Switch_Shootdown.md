# TLB Performance: Context Switch Overhead, Shootdown Costs, Optimization

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. TLB Performance Fundamentals

```
TLB performance is dominated by three costs:
  1. TLB miss cost (cold start or eviction)
  2. Context switch cost (with vs without ASID)
  3. TLB shootdown cost (mprotect, munmap, page migration)

TLB hit rate: what fraction of VA→PA lookups are found in the TLB
  Hit rate goal: > 99.9% (< 1 in 1000 accesses should miss)
  Below 98%: TLB misses become visible in profiling
  Below 95%: serious performance degradation

Why hit rate matters:
  Every memory access has an implicit TLB lookup.
  CPI (cycles per instruction): typically 1.0–2.0 for normal code
  TLB miss: 30–800 extra cycles
  At 1% miss rate: 0.01 × 400 cycles = 4 extra cycles/access
  → Effectively doubles average memory access cost for memory-bound workloads

Perf counters for TLB on ARM64:
  ITLB_WALK     (0x35): Instruction TLB walk (miss)
  DTLB_WALK     (0x34): Data TLB walk (miss)
  L1I_TLB_REFILL (0x01): L1 iTLB miss count
  L1D_TLB_REFILL (0x05): L1 dTLB miss count
  L2D_TLB_REFILL (0x2F): L2 TLB miss count
  L2D_TLB       (0x2E): L2 TLB access count
  
  L2 TLB miss rate = L2D_TLB_REFILL / L2D_TLB
  Useful: perf stat -e itlb_walk,dtlb_walk ./my_workload
```

---

## 2. Context Switch TLB Overhead Analysis

```
Workload: web server, 50,000 req/sec, each request spawns 1 thread

Scenario A: No ASID (hypothetical):
  Every context switch: TLBI VMALLE1IS → all TLB entries flushed
  Per switch cost: ~3 µs (flush + 100 TLB misses × 30 cycles each)
  Context switches per second: 100,000 (50,000 req × 2 switches each)
  
  Overhead: 100,000 × 3 µs = 300 ms/sec
  CPU utilization waste: 30% just for TLB management!
  Actual throughput: 70% of available CPU → cannot reach 50,000 req/sec

Scenario B: With ASID (ARM64 default):
  Context switch: MSR TTBR0_EL1, x0 + ISB = 5 cycles ≈ 0.0017 µs
  No TLB flush (each process has unique ASID)
  Old process's TLB entries persist (invisible, tagged with old ASID)
  
  If process was recently active: its TLB entries may still be in TLB
  → ZERO TLB misses on context switch (TLB warm)!
  
  TLB warmth benefit: a process that switches frequently keeps warm TLB
  Perfect for high-frequency context-switching workloads
  
  Overhead: 100,000 × 0.0017 µs = 0.17 ms/sec
  CPU utilization waste: 0.017% → negligible!

Conclusion: ASID is critical for workloads with high context switch rates.
```

---

## 3. TLB Shootdown Latency Measurement

```
mprotect() TLB shootdown cost on ARM64:

Single page, 4 cores:
  TLBI VAE1IS + DSB ISH:
  Measured: 1–3 µs on Cortex-A72 (1.5 GHz)
  Measured: 0.5–1.5 µs on Cortex-A78 (2.8 GHz)
  
1000 pages, 4 cores:
  Loop of 1000 × TLBI VAE1IS + DSB ISH:
  Without range TLBI: ~1000 × 2 µs = 2000 µs (2 ms!)
  With range TLBI (RVAAE1IS): ~3–10 µs

  Range TLBI provides 200–700× speedup for large range shootdowns!

Real-world scenario: JVM garbage collector
  GC scans heap, changes many PTEs from writable to read-only
  Without range TLBI: each page = 2µs → 100MB heap = 100MB/4KB × 2µs = 50 ms pause
  With range TLBI: same heap region = 5 µs → 10,000× improvement
  
  This is why ARMv8.4 FEAT_TLBRANGE was a significant GC performance improvement

Server scale: 128-core ARM Neoverse N2:
  TLBI VAE1IS + DSB ISH: ~15–30 µs (128 cores must acknowledge)
  TLBI RVAAE1IS (1GB range): ~20–40 µs
  
  At 128 cores, even ARM's hardware broadcast takes longer
  (interconnect mesh diameter larger, more acknowledgment hops)
```

---

## 4. Huge Pages for TLB Efficiency

```
TLB reach with different page sizes:

4KB pages, L2 TLB = 1024 entries:
  Reach = 1024 × 4KB = 4 MB
  Any working set > 4 MB → TLB misses

2MB pages (THP/HugeTLB), L2 TLB = 1024 entries:
  Reach = 1024 × 2MB = 2 GB
  Working set must exceed 2 GB before TLB pressure

1GB pages (HugeTLB), L2 TLB = 1024 entries:
  Reach = 1024 × 1GB = 1 TB
  Almost never TLB-limited!

Performance impact (database workload, 10GB working set):
  4KB pages: 10GB / 4MB = 2500 TLB misses per 4MB traversal
    Each access to a new 4KB page → TLB miss → 100–400 cycles
    
  2MB THP: 10GB / 2GB = 5× fewer TLB misses
    In practice: 5–15× speedup for TLB-bound workloads
    Measured: PostgreSQL 15% faster with THP
    Measured: Redis 10% faster with THP
    Measured: Oracle DB 5–20% faster depending on workload

Linux THP configuration:
  echo always > /sys/kernel/mm/transparent_hugepage/enabled
  → All eligible anonymous mappings get 2MB pages
  
  echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
  → Only regions with madvise(MADV_HUGEPAGE) use THP

HugeTLB (static huge pages):
  echo 1024 > /proc/sys/vm/nr_hugepages
  → Pre-allocate 1024 × 2MB = 2GB of huge pages
  Applications use these via mmap with MAP_HUGETLB flag
  Zero THP overhead (no splitting/merging, just pre-allocated)
```

---

## 5. TLB Performance Optimization Guidelines

```
For application developers:
  1. Use huge pages for large working sets:
     madvise(buf, size, MADV_HUGEPAGE) for frequently accessed buffers
     
  2. Minimize VA range fragmentation:
     Allocate large objects contiguously
     Avoid scattered small allocations in hot code paths
     
  3. Reduce page table depth where possible:
     Use 2MB or 1GB blocks → fewer TLB entries + fewer walk levels
     
  4. NUMA awareness: keep data on same NUMA node as CPU
     Cross-NUMA access doesn't help TLB but increases memory latency
     Combined effect is worse

For kernel/driver developers:
  1. Batch TLB invalidations:
     Use mmu_gather → defer TLBI until all PTEs updated
     One TLBI IS covers many pages vs. per-page TLBI
     
  2. Use range TLBI (ARMv8.4+):
     Detect FEAT_TLBRANGE and use RVAAE1IS for large ranges
     Avoid loops of per-page TLBI for > 64 pages
     
  3. Minimize page table modifications:
     Prefer larger block mappings (2MB) where possible
     Block mapping = 1 TLB entry for 512 small pages
     
  4. ASIDs for kernel threads:
     Pin kernel threads that access the same mm → avoid ASID-less flushes
     
  5. kfence / kasan: disable for performance-critical paths
     These add extra page table entries + TLB entries overhead
```

---

## 6. Profiling TLB Performance

```
Linux perf for TLB analysis:

Basic TLB miss rates:
  perf stat -e dtlb-loads,dtlb-load-misses,dtlb-stores,dtlb-store-misses \
           -e itlb_walk,dtlb_walk ./workload
  
  Output:
    100,000,000  dtlb-loads
         500,000  dtlb-load-misses   0.50% miss rate  ← healthy
    ...
    
    ARM64 PMU events:
    DTLB_WALK (0x34): data TLB walks started (misses)
    ITLB_WALK (0x35): instruction TLB walks started
    L2D_TLB_REFILL (0x2F): L2 TLB refills (PTW completed)

Flame graph for TLB miss analysis:
  perf record -e dtlb_walk:u -g ./workload
  perf report → shows which functions cause most TLB misses
  
  Hot functions = allocate scattered memory or access large data structures

vtune / ARM Streamline:
  Top-Down Microarchitecture Analysis: "Memory Bound" → "TLB Miss" category
  Shows: cycles wasted due to TLB miss, by function and source line

/proc/vmstat TLB flush counts:
  grep 'tlb' /proc/vmstat
  tlb_flush_reason_tlb_flush_on_task_switch: context switches
  tlb_flush_reason_remote_shootdown: cross-CPU TLB shootdowns
  High tlb_flush counts → potential optimization target
```

---

## 7. Interview Questions & Answers

**Q1: A database application is spending 30% of CPU time on TLB misses according to perf. What are the top 3 interventions you would try, in order?**

1. **Enable Transparent Huge Pages (THP)**: Use `madvise(buf, size, MADV_HUGEPAGE)` for the main database buffer pool (typically 10–100GB). Switching from 4KB to 2MB pages reduces TLB entries needed by 512×, turning TLB thrashing into near-zero TLB misses for the buffer pool. This is the highest-impact change, often yielding 10–30% performance improvement for databases.

2. **Use HugeTLB static huge pages** if THP allocation fragmentation is an issue: Pre-allocate `nr_hugepages` from the kernel and use `mmap(MAP_HUGETLB)` for the buffer pool. Static huge pages guarantee 2MB contiguous physical memory is available at startup, avoiding THP allocation failures during runtime.

3. **Optimize memory layout for TLB contiguous hint**: Ensure large sequential data structures are allocated aligned to 2MB boundaries so the hardware can use contiguous-hint optimization. Use `posix_memalign(ptr, 2MB, size)` for large arrays. Also check if the CPU supports 1GB pages for the buffer pool → 1 TLB entry per gigabyte of buffer pool.

---

## 8. Quick Reference

| Metric | Good | Warning | Critical |
|---|---|---|---|
| L1 dTLB miss rate | < 0.1% | 0.1–1% | > 1% |
| L2 TLB miss rate | < 0.01% | 0.01–0.1% | > 0.1% |
| Context switch time | < 5 µs | 5–50 µs | > 50 µs |
| TLB shootdown (per page) | < 2 µs | 2–10 µs | > 10 µs |

| Optimization | Benefit | Complexity |
|---|---|---|
| THP (2MB pages) | 5–15× TLB reach | Low (one madvise call) |
| HugeTLB (static) | 5–15× TLB reach | Medium (pre-allocation) |
| 1GB pages | 512× TLB reach | High (app changes needed) |
| Range TLBI | 200–700× shootdown speedup | Kernel-only |
| ASID (default) | Eliminates ctx-switch flush | Automatic |
