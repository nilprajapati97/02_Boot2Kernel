# Huge Pages on ARM64

**Category**: Huge Pages and Page Size Optimization  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Block Descriptors

```
ARM64 page table uses TWO types of descriptors:
  Page descriptor (L3): maps 4KB (with 4KB granule)
  Block descriptor (L1, L2): maps 1GB or 2MB directly

Block descriptor format at L2 (PMD level), 4KB granule:
  Bits [1:0] = 0b01 (block entry marker)
  Bits [47:21]: PA[47:21] (output address, 2MB aligned)
  Bits [63:52]: various attributes
  → Maps 2MB block directly (no L3 table needed)

Block descriptor at L1 (PUD level), 4KB granule:
  Bits [1:0] = 0b01 (block entry marker)
  Bits [47:30]: PA[47:30] (output address, 1GB aligned)
  → Maps 1GB block directly (no L2 or L3 needed)

Memory savings from huge pages:
  1000 × 4KB pages: need 1000 PTEs + L3 table (4KB) + L2 entry = ~12KB overhead
  1 × 2MB huge page: need 1 PMD entry = 8 bytes overhead
  TLB: 1 entry covers 2MB vs 1 entry per 4KB = 512× fewer TLB misses

ARM64 granule sizes:
  4KB granule:  CONFIG_ARM64_4K_PAGES (most common)
    L3 = 4KB pages, L2 = 2MB blocks, L1 = 1GB blocks
  16KB granule: CONFIG_ARM64_16K_PAGES
    L3 = 16KB pages, L2 = 32MB blocks, L1 = 64GB blocks
  64KB granule: CONFIG_ARM64_64K_PAGES
    L3 = 64KB pages, L2 = 512MB blocks
    No 1-level block (L1 not supported with 64KB)
  
  TCR_EL1.TG0: granule size for TTBR0 (user)
    0b00 = 4KB, 0b10 = 16KB, 0b01 = 64KB
  TCR_EL1.TG1: granule size for TTBR1 (kernel)
    0b10 = 4KB, 0b01 = 16KB, 0b11 = 64KB
```

---

## 2. Transparent Huge Pages (THP)

```
THP (Transparent Huge Pages): huge pages without explicit application changes

Normal huge pages (hugetlbfs):
  Application: must use mmap(MAP_HUGETLB) or shmat(SHM_HUGETLB)
  Kernel: pre-allocates huge pages in pool at boot
  Advantage: guaranteed availability, no fragmentation
  Disadvantage: application must explicitly request them

THP (transparent):
  Application: normal mmap() or malloc()
  Kernel: SILENTLY upgrades 4KB allocations to 2MB when:
    1. Request is 2MB-aligned, 2MB-size (PMD-aligned)
    2. Physical memory is available in 2MB contiguous chunks
    3. System THP policy allows it
  Application: never knows it got a huge page (transparent)
  
  THP control:
    /sys/kernel/mm/transparent_hugepage/enabled:
      always:  always try to use THP
      madvise: only where madvise(MADV_HUGEPAGE) was called
      never:   disable THP
    /sys/kernel/mm/transparent_hugepage/defrag:
      always:  run khugepaged defrag aggressively
      defer:   defer defrag to background (better latency)
      madvise: defrag only for MADV_HUGEPAGE regions
      never:   don't defrag for THP
  
  madvise(addr, length, MADV_HUGEPAGE): hint to kernel: use THP here
  madvise(addr, length, MADV_NOHUGEPAGE): hint: don't use THP here
  
  THP fault path:
    do_anonymous_page() → check if PMD-aligned, 2MB available
    →  __alloc_pages(GFP_KERNEL, order=9): 2MB compound page
    → pmd_set_huge(): install as PMD block entry (2MB block)
    ARM64: sets L2 block descriptor (bits[1:0]=0b01)
  
  THP and COW (Copy-On-Write):
    Fork: parent and child share the 2MB THP (read-only)
    Child writes: COW triggers
    2MB COW: must allocate new 2MB compound page → expensive
    Linux optimization: split THP to 4KB on COW if memory pressure
    thp_split_page(): splits 512 4KB pages, sets L3 PTEs

khugepaged daemon:
  Background thread: periodically scans for collapsible regions
  Finds: contiguous 512 × 4KB pages (2MB region, same VMA)
  Condition: all pages in region are:
    - Same owner (COW resolved)
    - Writable, anonymous
    - Resident in memory (none paged out)
  Action: khugepaged_collapse_huge_page():
    Allocate 2MB compound page
    Copy all 512 × 4KB pages to new 2MB page
    Replace 512 PTEs with 1 PMD block entry
    Free original 512 4KB pages
  
  khugepaged_pages_to_scan: how many pages to scan per run
  khugepaged_scan_sleep_millisecs: sleep between runs
```

---

## 3. hugetlbfs and Huge Page Pool

```
hugetlbfs: filesystem for explicit huge page management

Huge page pool:
  Pre-allocated at boot (or later via sysctl)
  /proc/sys/vm/nr_hugepages: number of pre-allocated 2MB huge pages
  /proc/sys/vm/nr_overcommit_hugepages: allow on-demand allocation
  /proc/hugepages: pool status
  
  Boot-time allocation:
    hugepages=512 on kernel command line: allocate 512 × 2MB huge pages
    Better: allocate at boot (memory not fragmented yet)
  
  Dynamic allocation:
    echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    May fail if memory is fragmented (can't find 512 contiguous pages)
  
  1GB huge pages on ARM64:
    /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
    Requires: physical 1GB-aligned, 1GB contiguous memory
    Very hard to allocate after boot (huge fragmentation concern)
    Best: hugepages=4 kernel cmdline parameter

Using hugetlbfs:
  mount -t hugetlbfs nodev /mnt/huge -o pagesize=2M
  Application:
    fd = open("/mnt/huge/file", O_CREAT | O_RDWR);
    addr = mmap(NULL, 2*1024*1024, PROT_RW, MAP_SHARED, fd, 0);
    → addr is backed by a 2MB huge page
  
  Or: anonymous huge pages:
    addr = mmap(NULL, 2*1024*1024, PROT_RW, MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
  
  hugetlbfs page table path:
    hugetlb_fault() → hugetlb_no_page() → alloc_huge_page()
    ARM64: install PMD block entry (for 2MB) or PUD block entry (for 1GB)

NUMA and huge pages:
  Per-NUMA-node huge page pools:
  /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
  Allocate huge pages on specific NUMA node
  Application: mbind() + MAP_HUGETLB for NUMA-local huge pages
```

---

## 4. TLB Efficiency with Huge Pages

```
TLB (Translation Lookaside Buffer) and huge page benefit:

4KB page workload:
  1GB working set → 262,144 TLB entries needed
  ARM64 L1 TLB: typically 48-64 entries
  Result: massive TLB thrashing (many misses, page table walks)
  
2MB huge page workload:
  1GB working set → 512 TLB entries needed
  Much more likely to fit in L1 TLB
  Result: dramatically fewer TLB misses

Specific TLB capacity (representative ARM64 CPUs):
  Cortex-A76:
    L1 I-TLB: 48 entries (4KB, 2MB, 1GB)
    L1 D-TLB: 48 entries
    L2 TLB: 1280 entries (unified, 4KB+2MB+1GB)
  
  Neoverse N2:
    L1 I-TLB: 48 entries
    L1 D-TLB: 48 entries
    L2 TLB: 2048 entries
  
  With 2MB huge pages:
    Neoverse N2 L2 TLB: 2048 entries × 2MB = 4GB coverage!
    vs 4KB: 2048 × 4KB = 8MB coverage only

Performance gains from THP:
  Database workloads: 2-20% speedup (InnoDB, PostgreSQL)
  JVM workloads: 2-10% speedup
  HPC / numerical: 5-30% speedup (sequential access patterns)
  
  Negative cases:
    Highly fragmented access: huge pages waste memory
    malloc + free intensive: THP fragmentation hurts
    Small processes: THP overhead > benefit

ARM64 huge page TLB attributes:
  2MB block entry in L2: tagged in TLB with page size indicator
  TLB hardware: caches block entries differently
  TLBI instructions respect block entries:
    TLBI VAE1IS, <VA>>12>: invalidates by VA regardless of page size
    Hardware: finds matching block TLB entry and invalidates it
    No need for caller to know if it's 4KB or 2MB
```

---

## 5. ARM64 16KB and 64KB Page Granules

```
ARM64 supports three page granule sizes: 4KB, 16KB, 64KB

16KB granule (CONFIG_ARM64_16K_PAGES):
  L3 page: 16KB (not 4KB)
  L2 block: 32MB (2^13 × 16KB)
  L1 block: 64GB (2^13 × 32MB)  [ARMv8.2+ FEAT_LPA needed for full range]
  
  Page table size: 16KB per level (2048 entries × 8B)
  ARM64 hardware: natively supports 16KB granule
  
  Advantages:
    Better for MMIO: 16KB granule reduces page table overhead for aligned MMIO
    Larger TLB reach: each L2 block = 32MB vs 2MB with 4KB granule
    Fewer page faults: fewer pages to fault in for large allocations
  
  Disadvantages:
    16KB alignment for all allocations (wasted memory for small)
    Linux: malloc() allocations still aligned to 16KB minimum
    4KB-aligned shared libraries: may need padding/alignment fixes
  
  Usage: Apple Silicon Macs run macOS with 16KB pages
         Linux on M1/M2: uses 16KB granule for native ARM64 kernel

64KB granule (CONFIG_ARM64_64K_PAGES):
  L3 page: 64KB
  L2 block: 512MB
  No L1 blocks (hardware limitation with 64KB granule)
  
  Page table size: 64KB per level (8192 entries × 8B)
  
  Advantages:
    Huge performance benefit for NUMA/server: 512MB block entries in TLB
    Fewer page table levels needed (3 levels covers 48-bit VA)
    Large MMIO regions: single L2 entry for 512MB BAR
    
  Disadvantages:
    64KB minimum page: any mmap'd file wastes up to 63KB
    Shared libraries: page-aligned sections may have large gaps
    Many external library assumptions about 4KB pages
  
  Usage: IBM POWER machines use 64KB (historical)
         ARM64 server use: rare (4KB preferred for compatibility)
         HPC specialized: 64KB can give massive TLB benefit

Choosing granule (ARM64 server platforms):
  4KB: best compatibility, widest hardware support, fine granularity
  16KB: Apple Silicon (M1/M2/M3), slightly better huge page coverage
  64KB: maximum TLB efficiency, but compatibility concerns

TCR_EL1 configuration for each granule:
  4KB:  TCR_EL1.TG0=0b00, TCR_EL1.TG1=0b10
  16KB: TCR_EL1.TG0=0b10, TCR_EL1.TG1=0b01
  64KB: TCR_EL1.TG0=0b01, TCR_EL1.TG1=0b11
```

---

## 6. Interview Questions & Answers

**Q1: What is the difference between THP, hugetlbfs, and 64KB granule?**

**THP (Transparent Huge Pages)**: automatic, software-driven. Kernel silently upgrades anonymous pages to 2MB when conditions are right. No application change. Uses 2MB PMD-level block entries. Relies on `khugepaged` for proactive collapsing. Fallback: splits back to 4KB if needed.

**hugetlbfs**: explicit, application must request huge pages. Pre-allocated pool. Guaranteed availability (no fallback). Supports 1GB pages. Used for: databases (Oracle, PostgreSQL), DPDK, JVM large heaps.

**64KB granule**: hardware-level choice. Every page is 64KB. TLB entries cover 64KB minimum. Does NOT require any userspace change but does affect page alignment. Not "huge pages" in the traditional sense — it's just a different base page size. All existing huge page mechanisms (THP, hugetlbfs) still work on top of 64KB granule (block entries = 512MB or 32MB with 64KB granule).

**Q2: How does ARM64 represent a 2MB huge page in the page tables?**
At L2 (PMD) level, instead of a table descriptor pointing to an L3 page table, a 2MB block descriptor is installed. Bits[1:0] = 0b01 (block entry, not table=0b11). Bits[47:21] hold PA[47:21] (output address, 2MB aligned). Upper attribute bits: same as regular 4KB PTE (access permissions, memory attributes, shareability). One L2 PMD entry covering all 512 × 4KB sub-pages. Linux: `pmd_set_huge()` sets this; `pmd_huge()` tests for it.

**Q3: When would you NOT use THP for a workload?**
THP has pathological cases: (1) **Heap-intensive allocators**: malloc/free creates many small objects — THP causes internal fragmentation (full 2MB allocated for a few KB of actual data). (2) **Latency-sensitive apps**: THP allocation (order-9 alloc) and `khugepaged` defragmentation create latency spikes (allocation failures → compact_zone() → long pause). (3) **Already-fragmented memory**: THP just can't allocate in a fragmented system — constant compaction overhead. (4) **Copy-on-fork workloads**: forking with THP = expensive 2MB COW copies. For these, `madvise(MADV_NOHUGEPAGE)` or `THP=never` is better.

**Q4: What is khugepaged and what does it do?**
`khugepaged` is a kernel daemon (`kthread`) that proactively collapses 4KB page ranges into 2MB THP. Algorithm: scan VMAs looking for 512 consecutive 4KB pages (one 2MB region) where all pages are: mapped, anonymous, writable, same owner (COW resolved), resident. If found: allocate one 2MB compound page, copy all 512 × 4KB pages, replace 512 PTEs with one PMD block entry, free the 512 4KB pages. Result: application now uses huge pages without having faulted one initially. Controlled via: `/sys/kernel/mm/transparent_hugepage/khugepaged/`.

**Q5: How do 16KB pages affect Linux compatibility on ARM64 (Apple M1)?**
Apple Silicon Macs use 16KB pages. Main compatibility issues: (1) Binaries assuming `getpagesize()=4096` may have alignment issues (fixed in most modern apps). (2) `mmap()` with `MAP_FIXED` at non-16KB-aligned addresses fails. (3) Shared libraries with `PT_LOAD` segments at 4KB-page boundaries → linker must pad to 16KB. (4) Memory-mapped file at byte offset not 16KB-aligned → needs adjustment. Linux on M1 with 16KB granule: mostly compatible — most apps use `sysconf(_SC_PAGESIZE)` correctly. Specific issue: some older apps hardcode `4096` → may need patches.

---

## 7. Quick Reference

| Granule | L3 Page | L2 Block | L1 Block | TLB entries for 1GB |
|---|---|---|---|---|
| 4KB | 4KB | 2MB | 1GB | 512 (2MB) or 1 (1GB) |
| 16KB | 16KB | 32MB | 64GB | ~32 (32MB) |
| 64KB | 64KB | 512MB | N/A | ~2 (512MB) |

| Huge Page Method | Granularity | API | Guaranteed? |
|---|---|---|---|
| THP | 2MB (auto) | mmap() + madvise | No (may fall back) |
| hugetlbfs 2MB | 2MB | mmap(MAP_HUGETLB) | Yes (if pool available) |
| hugetlbfs 1GB | 1GB | mmap(MAP_HUGETLB+1GB) | Yes (if pool available) |
| 64KB granule | 64KB base page | No change needed | Always (HW) |
