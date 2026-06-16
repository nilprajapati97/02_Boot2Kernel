# NUMA Architecture and Multi-Core Memory Management

**Category**: NUMA and Multi-Core Memory  
**Platform**: ARM64 (AArch64)

---

## 1. NUMA Concept

```
NUMA (Non-Uniform Memory Access): memory topology where access latency 
depends on physical location relative to the accessing CPU

UMA (Uniform Memory Access, single-node):
  All CPUs: same distance to all memory
  Latency: ~50ns for all accesses
  Simple: no NUMA topology to consider

NUMA (multi-node):
  CPU cluster A → local memory: ~50ns
  CPU cluster A → remote memory: ~100-200ns
  NUMA ratio: 2-4× slower for remote access
  
  ARM64 NUMA examples:
    Neoverse N1 system (2-socket): 2 NUMA nodes
    Ampere Altra Max: single socket, but internal NUMA (mesh fabric)
    Amazon Graviton3: potentially NUMA (128 cores, mesh fabric)
  
  ARM64 NUMA topology:
    ACPI SRAT table: declares CPU-to-memory-node affinity
    ACPI SLIT table: distance between nodes (latency matrix)
    Device tree: numa-node-id property for CPUs and memory

NUMA data structures in Linux:
  struct pglist_data (pg_data_t):
    Each NUMA node has ONE struct pglist_data
    Fields:
      node_id:          NUMA node number (0, 1, ...)
      node_start_pfn:   first page frame in this node
      node_present_pages: usable pages in node
      node_spanned_pages: total span (including holes)
      node_mem_map:     vmemmap pointer for this node
      node_zones[]:     array of zones (DMA, NORMAL, MOVABLE)
      kswapd:           per-node kswapd kthread
      kswapd_classzone_idx: zone that triggered kswapd
      lruvec (in zone): LRU lists for page reclaim
  
  num_online_nodes(): how many NUMA nodes
  NODE_DATA(nid): get pg_data_t for node nid
  
  Per-node zone structure:
    Node 0: ZONE_DMA (0-4GB), ZONE_NORMAL (4GB+), ZONE_MOVABLE
    Node 1: ZONE_NORMAL, ZONE_MOVABLE (no DMA zone on non-zero nodes typically)
```

---

## 2. ARM64 NUMA Hardware Topology

```
ARM64 server NUMA topology (Neoverse N1/N2):

Single socket, CMN-700 mesh:
  4 clusters × 4 cores = 16 cores total
  CMN-700 (Coherent Mesh Network):
    8×8 or 4×4 mesh of nodes
    Each node: CHI home node (HN-F), or CPU cluster (RN-F), or memory
    Memory controllers: distributed across mesh
    Local memory: ~50ns (CPU to nearest HN-F to nearest DDR)
    Cross-mesh memory: ~80-100ns
    Linux: can configure as 1 NUMA node or 4 NUMA "nodes" (clusters)
  
  Real 2-socket example (Neoverse N2):
    Socket 0: 128 cores + 4 × DDR5 channels → Node 0
    Socket 1: 128 cores + 4 × DDR5 channels → Node 1
    Inter-socket: PCIe link or coherent interconnect (CCIX/CXL)
    Distance: local=10, remote=30 (from SLIT table)
  
  ACPI SRAT (System Resource Affinity Table) entry:
    Memory Affinity Entry:
      Base Address: physical start of memory
      Length: size of this memory range
      Proximity Domain: NUMA node ID
    CPU Affinity Entry (GICC type):
      APIC ID: CPU ID
      Proximity Domain: which NUMA node this CPU belongs to
  
  Reading NUMA topology:
    numactl --hardware:
      available: 2 nodes (0-1)
      node 0 cpus: 0-63
      node 0 size: 256GB
      node 0 free: 200GB
      node distances:
        node   0   1
          0:  10  30
          1:  30  10
    
    /sys/devices/system/node/: per-node sysfs
    /sys/devices/system/node/node0/cpumap: bitmask of CPUs in node 0
    /sys/devices/system/node/node0/meminfo: memory stats for node 0
```

---

## 3. NUMA-Aware Allocation

```
NUMA page allocation policy:

Default policy (MPOL_DEFAULT):
  Allocate on the node of the faulting CPU
  "First-touch policy": whichever CPU first faults a page owns it
  
Process NUMA policies (set_mempolicy()):
  MPOL_BIND:       allocate only from specified node(s) — strict
  MPOL_PREFERRED:  prefer specified node, fall back to others
  MPOL_INTERLEAVE: round-robin across nodes (good for bandwidth-heavy)
  MPOL_DEFAULT:    use first-touch
  MPOL_LOCAL:      allocate from node of requesting CPU
  
numactl command:
  numactl --cpunodebind=0 --membind=0 ./myapp
  numactl --interleave=all ./db_server
  
VMA-level NUMA policy:
  mbind(addr, length, MPOL_BIND, nodemask, ...): set NUMA policy for range
  Individual mmap ranges can have different NUMA policies

Buddy allocator NUMA paths:
  alloc_pages(gfp_flags, order):
    = alloc_pages_current() for current process policy
    = alloc_pages_node(nid, gfp_flags, order) for explicit NUMA node
  
  __alloc_pages_nodemask():
    1. Try NUMA policy preferred node first
    2. If fail: try other nodes (cpuset fallback)
    3. If all fail: OOM
  
  get_page_from_freelist():
    For each preferred zone/node: try zonelist in order
    zonelist: ordered list of (zone, node) pairs per NUMA policy
    Populated by: build_zonelists() → ordered by proximity

Per-CPU caches (PCP) and NUMA:
  Per-CPU page caches: pcpu_list[cpu] → hot/cold page lists
  Local allocation: always fill from local node PCP first
  Cross-node: only if local node is empty
  Benefit: CPU-local allocation = local memory (NUMA-friendly)
```

---

## 4. Auto-NUMA (NUMA Balancing)

```
Auto-NUMA: kernel automatically migrates pages to local memory

Problem: process migrated to CPU on remote node
  Pages: still on original node (local to original CPU)
  Access: now remote memory (slow)
  
NUMA balancing solution:
  Kernel: periodically scans process's pages
  Mark: accessed PTEs as "NUMA hint" (clear access flag + mark as NUMA fault)
  On next access: NUMA hint fault fires
  Kernel: decides whether to migrate page to current node
  Migration: move page data from remote node to local node
             update PTE to new physical address
  Result: process now accesses local memory (fast)

NUMA balancing configuration:
  /proc/sys/kernel/numa_balancing: 0=disable, 1=enable
  /proc/sys/kernel/numa_balancing_scan_delay_ms: delay between scans
  /proc/sys/kernel/numa_balancing_scan_period_min_ms: minimum scan period
  /proc/sys/kernel/numa_balancing_scan_size_mb: scan window per period
  
  NUMA balancing overhead:
    Scanning: slight CPU overhead for PTE scanning
    Faults: NUMA hint faults add latency on first access
    Migrations: page migration = read old + write new + TLB flush
    Net benefit: large for long-running processes with fixed NUMA affinity
  
NUMA fault handling:
  numa_migrate_prep(): prepare for NUMA migration
    Check: is target page already local? If so: just update affinity stats
    Check: will migration benefit? (enough misses to justify)
  
  migrate_pages():
    1. isolate pages from LRU
    2. alloc_pages_node(target_node) = allocate on target
    3. copy_highpage(new, old): copy data
    4. replace_page_cache(): update mapping
    5. migrate_page_move_mapping(): remap PTE to new PA
    6. TLB flush (TLBI ASIDE1IS for ASID)
    7. free old pages

NUMA scheduling and memory together:
  task_numa_fault(): called on NUMA hint fault
    Tracks: which node the thread is accessing most
    Suggests: schedule_numa_preferred(): migrate thread to best node
              OR: migrate page to current node
    Decision: based on numa_scan_period counts, node_load
  
  task_work:
    select_task_rq_fair(): NUMA-aware scheduler placement
    Prefers: run thread on node where most memory is local
```

---

## 5. NUMA Page Migration

```
Page migration: moving physical page content from one NUMA node to another

migrate_pages() kernel function:
  Input: list of pages to migrate, target node/allocation function
  
  Steps for each page:
    1. trylock_page(): get exclusive lock on page
    2. Remove from page cache / address_space
    3. Wait for writeback if dirty
    4. alloc_page(target): allocate on target node
    5. copy_highpage(newpage, page): copy data (memcpy)
    6. Remap: all PTEs pointing to old page → new page
       For each mapped VMA:
         pte_offset_map(), ptep_clear_flush(), set_pte_at(new_pte)
         TLBI ASIDE1IS (or VAE1IS): flush TLB for old mapping
    7. Free old page: put_page()
    
  Pitfalls:
    mlocked pages: cannot migrate
    Pages with elevated refcount: cannot migrate
    KSM pages: need special handling
    Huge pages: migrate_huge_page() → 2MB copy
  
  Measurement:
    /proc/<pid>/numa_maps: per-VMA NUMA stats
      7f8000000000 default file=... mapped=100 mapmax=100 N0=80 N1=20
      80% pages on node 0, 20% on node 1
    /sys/devices/system/node/node0/numastat:
      numa_hit:         allocations that succeeded on preferred node
      numa_miss:        allocations that had to go to other nodes
      numa_foreign:     pages allocated here but preferred elsewhere
      interleave_hit:   interleaved allocations on this node
      local_node:       allocations by CPUs on this node
      other_node:       allocations by CPUs on other nodes

Memory binding for performance:
  DB server: bind shared memory to local node
    mmap(MAP_SHARED, ...) + mbind(MPOL_BIND, node0_mask)
  
  Multi-threaded: interleave for balanced bandwidth
    numactl --interleave=all: allocate pages round-robin across nodes
    Good for: in-memory databases, scientific computing

  Dedicated allocation: MPOL_BIND + MPOL_STRICT
    Fail allocation if target node is out of memory
    Rather than silently use wrong node
```

---

## 6. Interview Questions & Answers

**Q1: What is NUMA and why does it matter for ARM64 servers?**
NUMA (Non-Uniform Memory Access) refers to memory topology where CPUs have different latency to different memory regions. ARM64 server chips (Neoverse N1/N2) use mesh interconnects (CMN-700): memory controllers are distributed, and CPUs closer to a memory controller have lower latency (~50ns local vs ~80-100ns cross-mesh). On 2-socket systems: NUMA ratio is 2-3× slower for remote memory. This matters because: workloads that allocate memory on one socket but process it on another suffer significantly. Linux NUMA-aware allocation and the Auto-NUMA scheduler help keep data local to the processing CPU.

**Q2: What is the "first-touch" NUMA policy?**
The default NUMA policy (`MPOL_DEFAULT`) allocates pages on the NUMA node of the CPU that first touches (faults) the page. `malloc()` returns virtual addresses — no physical allocation yet. The first `write` to each page causes a fault, allocates a physical page on the faulting CPU's node. If thread 0 (on node 0) initializes an array: all pages land on node 0. If thread 1 (on node 1) later processes the array: all accesses are remote. Solution: have each thread initialize its own portion of the data (parallel init pattern) so pages land on the "right" node.

**Q3: How does Auto-NUMA work?**
Auto-NUMA periodically marks process PTEs as "NUMA hints" (soft-clears the access flag). On next access, a NUMA hint fault fires. `task_numa_fault()` records which node the fault occurred from. After enough faults, Linux decides: either migrate the page to the current node (if the thread consistently accesses it from a different node) or migrate the thread to the node where most of its memory lives. The goal: maximize "local node accesses" / "total accesses" ratio. Can be disabled for latency-sensitive workloads: `/proc/sys/kernel/numa_balancing=0`.

**Q4: What is the difference between MPOL_BIND, MPOL_PREFERRED, and MPOL_INTERLEAVE?**
`MPOL_BIND`: allocate ONLY from specified nodes. If those nodes are full: allocation fails (or OOM) — strict. Used when correctness or predictability matters more than availability. `MPOL_PREFERRED`: try specified node first, fall back to any other node on failure. Best effort. `MPOL_INTERLEAVE`: round-robin allocation across specified nodes. Each page goes to the next node in sequence. Best for: workloads that access all data equally — maximizes total memory bandwidth by using all nodes' memory controllers simultaneously. Example: `numactl --interleave=all` for in-memory databases.

**Q5: How does page migration handle TLB consistency on ARM64?**
After migrating page data to a new physical address: 1. The old PTE must be removed and new PTE installed: `ptep_clear_flush(vma, vaddr, pte)` (clears PTE and does local TLB flush). 2. New PTE installed: `set_pte_at(mm, vaddr, pte_ptr, new_pte)`. 3. Full TLB broadcast: `flush_tlb_page(vma, vaddr)` → on ARM64 = `TLBI VAE1IS, <vaddr>>12>` (Inner Shareable broadcast). 4. DSB ISH: wait for all CPUs to complete TLB invalidation. Only after TLB is fully clean: the old physical page can be freed. Any CPU still holding the old TLB entry would access freed/reused memory (use-after-free at hardware level).

---

## 7. Quick Reference

| NUMA Policy | Behavior | Use Case |
|---|---|---|
| MPOL_DEFAULT | First-touch allocation | Default |
| MPOL_BIND | Strict: specified nodes only | Deterministic latency |
| MPOL_PREFERRED | Prefer node, fall back | Soft affinity |
| MPOL_INTERLEAVE | Round-robin across nodes | Maximize bandwidth |
| MPOL_LOCAL | Always allocate local | Low-latency per-thread |

| NUMA Statistic | What It Measures |
|---|---|
| numa_hit | Allocations on preferred node |
| numa_miss | Allocations that fell back to other nodes |
| numa_foreign | Allocations by remote CPUs |
| local_node | Allocations by local CPUs |
