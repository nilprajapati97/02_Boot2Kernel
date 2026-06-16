# NUMA Architecture and Allocation on ARM64

**Category**: NUMA and Multi-Core Memory  
**Platform**: ARM64 (AArch64) — Neoverse, Ampere, multi-socket servers

---

## 1. Concept Foundation

```
NUMA (Non-Uniform Memory Access):
  Problem: as CPU counts grow, all CPUs cannot share one memory bus
  Solution: each CPU (or CPU group) has local memory bank
            accessing local memory: fast (low latency)
            accessing remote memory: slow (cross-interconnect)
  
  ARM64 NUMA example: 2-socket Neoverse N2 server
    Socket 0: 64 Neoverse N2 cores + 512GB DDR5 (local)
              CMN-700 mesh interconnect (Coherent Mesh Network)
    Socket 1: 64 Neoverse N2 cores + 512GB DDR5 (local)
              CMN-700 mesh interconnect
    Cross-socket: PCIe/CXL or proprietary die-to-die link
    
    Local access latency:  ~70ns
    Remote access latency: ~140ns (2× slower cross-socket)
  
  Software goal: allocate memory LOCAL to the CPU that will use it
                 avoid remote accesses as much as possible
```

---

## 2. ARM64 NUMA Topology Discovery

```
ACPI SRAT (System Resource Affinity Table):
  BIOS/firmware provides SRAT to describe NUMA topology
  
  SRAT entries:
    Processor Local APIC/x2APIC Affinity:
      Proximity Domain (NUMA node ID) → APIC ID mapping
      In ARM64: Processor Local GIC (GICC) Affinity Structure
        Proximity Domain [31:0]: NUMA node number
        MPIDR [63:0]: CPU affinity (matches CPU's MPIDR_EL1)
    
    Memory Affinity Structure:
      Base Address: PA start of memory region
      Length: size of memory region
      Proximity Domain: NUMA node that owns this memory
      Flags: Enabled, Hot-Pluggable, Non-Volatile
    
  SLIT (System Locality Information Table):
    Matrix of relative latencies between NUMA nodes
    SLIT[i][j] = relative latency from node i to node j
    Value 10 = local access, 20 = 2x slower, etc.
  
  Linux parsing:
    acpi_numa_init()
      → acpi_table_parse_srat(ACPI_SRAT_TYPE_MEMORY_AFFINITY, ...)
      → acpi_table_parse_srat(ACPI_SRAT_TYPE_GIC_ITS_AFFINITY, ...)
      → numa_add_memblk(nid, start, end): maps PFN range → NUMA node
      → acpi_parse_slit(): stores SLIT matrix in node_distance[i][j]

kernel data structures:
  node_data[MAX_NUMNODES]:  array of struct pglist_data*, one per node
  node_to_cpumask_map[]:    which CPUs are on which node
  numa_distance[][]:         latency matrix from SLIT
  memblock regions:         each region has .nid field = NUMA node
```

---

## 3. struct pglist_data — Per-Node Memory Structure

```c
// include/linux/mmzone.h
struct pglist_data {
    // Per-node zone information:
    struct zone node_zones[MAX_NR_ZONES];
    // zone types: ZONE_DMA, ZONE_DMA32, ZONE_NORMAL, ZONE_MOVABLE
    // On ARM64: typically ZONE_DMA32 (0-4GB) + ZONE_NORMAL (4GB+)
    
    struct zonelist node_zonelists[MAX_ZONELISTS];
    // Ordered list of zones for allocation fallback:
    // ZONELIST_FALLBACK: local zone first, then remote zones (by distance)
    // ZONELIST_NOFALLBACK: local zones only (GFP_THISNODE)
    
    int nr_zones;             // number of active zones
    
    unsigned long node_start_pfn;   // first PFN on this node
    unsigned long node_spanned_pages; // total pages (including holes)
    unsigned long node_present_pages; // actual present pages
    
    int node_id;              // NUMA node number
    
    // kswapd (per-node memory reclaim daemon):
    wait_queue_head_t kswapd_wait;
    wait_queue_head_t pfmemalloc_wait;
    struct task_struct *kswapd; // kswapd thread for this node
    int kswapd_order;           // page order kswapd is trying to free
    
    // Memory compaction:
    unsigned long flags;
    
    // Statistics:
    struct per_cpu_nodestat __percpu *per_cpu_nodestats;
    atomic_long_t vm_stat[NR_VM_NODE_STAT_ITEMS];
    
    // LRU lists (in newer kernels, inside struct lruvec):
    struct lruvec __lruvec;
};

// Access:
NODE_DATA(nid)  →  node_data[nid]  (pointer lookup)
```

---

## 4. NUMA-Aware Buddy Allocator

```
alloc_pages_node(nid, gfp_mask, order):
  Explicitly allocate from NUMA node 'nid'
  
alloc_pages(gfp_mask, order):
  Uses current CPU's NUMA node (numa_node_id())
  With fallback: tries preferred node first, then fallback zonelist
  
alloc_pages_nodemask(gfp_mask, order, preferred_nid, nodemask):
  Full-featured: start from preferred_nid, follow fallback zonelist
  Constrained to nodemask if provided (from MPOL_BIND policy)

Zonelist fallback order (for node 0 in a 2-node system):
  1. NODE 0, ZONE_NORMAL      (local, preferred)
  2. NODE 0, ZONE_DMA32       (local, lower zone fallback)
  3. NODE 1, ZONE_NORMAL      (remote node, same zone type)
  4. NODE 1, ZONE_DMA32       (remote node, lower zone)
  
  build_zonelists() called at boot:
    Sorts nodes by node_distance() from local node
    Closer nodes come first in fallback list

GFP flags for NUMA:
  __GFP_THISNODE: MUST allocate from specified node (no fallback)
  __GFP_RECLAIM: allow reclaim if local node full
  No __GFP_THISNODE + no nodemask: full fallback chain
  GFP_KERNEL = __GFP_RECLAIM | __GFP_IO | __GFP_FS  (with fallback)
```

---

## 5. NUMA Memory Policies

```c
// User-space API: set_mempolicy() + mbind()
// include/uapi/linux/mempolicy.h

MPOL_DEFAULT:
  Use thread's current allocation preference (default: local node)
  Fallback to remote nodes if local node full

MPOL_BIND:
  struct mempolicy.v.nodes = specified nodemask
  MUST allocate from one of the specified nodes
  No fallback beyond the nodemask
  Use case: latency-sensitive code that cannot tolerate remote access

MPOL_PREFERRED:
  Prefer specified node, but fallback to others if needed
  Softer than MPOL_BIND

MPOL_PREFERRED_MANY (Linux 5.15+):
  Like MPOL_PREFERRED but with a set of preferred nodes
  Useful for heterogeneous NUMA (mix of local + near-local nodes)

MPOL_INTERLEAVE:
  Round-robin allocation across specified nodes
  Use case: shared data structures accessed equally by all CPUs
  Result: lower peak latency variance (no hot node)

MPOL_LOCAL (Linux 4.9+):
  Always allocate from current thread's local node
  Ignore all other settings
  Fastest for CPU-local data

Kernel representation:
  struct mempolicy {
    atomic_t refcnt;
    unsigned short mode;      // MPOL_* value
    unsigned short flags;
    nodemask_t nodes;         // for BIND/PREFERRED/INTERLEAVE
    union {
        nodemask_t cpuset_mems_allowed; // for cpuset interaction
        nodemask_t user_nodemask;
    };
  };
  
  VMA association:
    vma->vm_policy: per-VMA policy (overrides process policy)
    mpol_shared_policy: for shared mappings (SHM, mmap shared)
  
  Thread association:
    task->mempolicy: process-wide policy
```

---

## 6. Auto-NUMA (NUMA Balancing)

```
Goal: automatically migrate pages to node where they are most accessed
Activation: kernel.numa_balancing sysctl = 1 (or boot param)
            CONFIG_NUMA_BALANCING=y

Phase 1: Access pattern detection
  numa_balancing_scan_period_min/max: how often to scan per-process VMAs
  
  task_numa_work():
    Called from scheduler tick (every N ms per process)
    Walks process VMAs: marks random subset of PTEs as PROT_NONE
    These pages become "unmapped" temporarily
    
    ARM64: clear PTE bits[7:6] (AP field) or set UXN/PXN to cause fault
    Actually: change PTE to have no read/write permissions → any access faults
    
    pte_make_numa(pte):
      = set PTE_SPECIAL + clear PTE_PRESENT (on some architectures)
      ARM64: uses a specific pte encoding that causes fault but is recognized
             as NUMA probe (not a real page fault)

Phase 2: Fault collection
  do_numa_page():
    Called when NUMA-probe PTE faulted (access to PROT_NONE page)
    
    Restore PTE (re-enable access — this is just probing):
      pte_mknormal(pte)
    
    Record: which NUMA node this CPU is on → task_numa_fault()
    
    task_numa_fault(last_cpupid, nid, pages, flags):
      Checks: is the page on the SAME node as the CPU? → local access
              is the page on a DIFFERENT node? → remote access
      
      If too many remote accesses:
        task_numa_migrate(tsk): trigger page migration
        numa_migrate_prep(): check if migration is beneficial

Phase 3: Page Migration decision
  numa_migrate_prep():
    Compare: nr_local_faults vs nr_remote_faults
    If remote_faults > threshold → migrate page to current CPU's node
    
    migrate_pages(pagelist, alloc_page_noprof, nid, ...):
      1. Isolate page from LRU: isolate_lru_page()
      2. Allocate replacement on target node: alloc_pages_node(target_nid)
      3. Copy page content: copy_highpage(newpage, oldpage)
      4. Update page table: migrate_entry_wait (in pte replacement)
      5. ARM64: TLB flush required after PTE update:
           flush_tlb_page(vma, addr)
           ARM64: TLBI VAE1IS, Xt (per-page TLB invalidation)
      6. Free old page: put_page(oldpage)
      
  Result: page now lives on node where it is most accessed

Phase 4: Task placement
  scheduler NUMA placement (select_task_rq_fair()):
    When waking task: prefer CPU on node with most of task's memory
    numa_prefer_nid(p): find best NUMA node for this task
    Balance: memory locality vs load balancing
    
    task_struct NUMA fields:
      p->numa_preferred_nid: preferred NUMA node
      p->numa_faults[]: fault statistics per node
      p->total_numa_faults: total NUMA fault count
      p->numa_scan_period: current scan period (ms)
```

---

## 7. Interview Q&A

**Q1: How does ARM64 indicate NUMA topology to Linux?**
ARM64 uses ACPI SRAT (System Resource Affinity Table) for NUMA discovery. The SRAT contains: (1) Processor affinity structures mapping MPIDR_EL1 values (CPU IDs) to Proximity Domains (NUMA node numbers). (2) Memory affinity structures mapping physical address ranges to Proximity Domains. (3) SLIT table provides relative latency values between nodes (e.g., 10=local, 20=remote). Linux parses these in `acpi_numa_init()`, calls `numa_add_memblk()` to tag memblock regions with their node ID, and builds `node_distance[][]` from SLIT. Result: each CPU knows its node, each physical address range is tagged with a node.

**Q2: What is the fallback order in the buddy allocator for NUMA allocations?**
The buddy allocator uses a `zonelist` (ordered list of zones) per NUMA policy. Default fallback: (1) Local node, highest zone (ZONE_NORMAL). (2) Local node, lower zones (DMA32). (3) Nearest remote node (by SLIT latency), same zone type. (4) Nearest remote node, lower zones. (5) Further remote nodes in increasing latency order. Built by `build_zonelists()` at boot. `GFP_THISNODE` skips all remote fallback (may fail if local node is exhausted). Most allocations: `GFP_KERNEL` — allows remote fallback to avoid allocation failure at the cost of higher latency.

**Q3: How does Auto-NUMA detect memory access patterns?**
Auto-NUMA uses a sampling approach: `task_numa_work()` (called from scheduler ticks) walks a subset of the process's PTEs and temporarily makes them PROT_NONE (no-access). The next access to these pages faults. `do_numa_page()` handles these NUMA probe faults: it records WHICH NUMA node the faulting CPU is on and WHICH node the page currently lives on. After collecting N faults, it compares local vs remote fault counts. If mostly remote: `migrate_pages()` moves the page to the current CPU's node. The PROT_NONE trick is the key insight: it turns memory accesses into traceable events without hardware performance counters.

**Q4: What is the ARM64 TLB flush requirement during NUMA page migration?**
When `migrate_pages()` replaces a page table entry (old PTE pointing to old_pfn replaced by new PTE pointing to new_pfn), the old TLB entry must be invalidated. ARM64: `flush_tlb_page(vma, addr)` issues `TLBI VAE1IS, Xt` (TLB Invalidate by VA, EL1, Inner Shareable). The IS (Inner Shareable) broadcast ensures all CPUs in the cluster see the invalidation (critical for multi-socket: other CPUs may have cached the old PTE). Failure to flush: CPUs would continue accessing old_pfn (which may be freed and reallocated to a different process) → data corruption or security violation.

**Q5: When should MPOL_BIND be preferred over MPOL_PREFERRED?**
MPOL_BIND: allocation MUST come from specified nodes; fails (ENOMEM) rather than using remote memory. Use when: latency is strictly bounded (real-time or near-real-time applications), memory bandwidth is the bottleneck (cross-socket bandwidth is half local bandwidth on many ARM64 servers), or correctness depends on local access (e.g., lock-free algorithms with memory ordering assumptions). MPOL_PREFERRED: try the specified node, fall back to others. Use when: application performance degrades with remote access but must not fail; interactive workloads; when memory overcommit is acceptable. MPOL_BIND is stricter and can cause OOM on NUMA-constrained allocation requests.

---

## 8. Quick Reference

| Policy | Behavior | Use Case |
|---|---|---|
| `MPOL_DEFAULT` | Local node, full fallback | General use |
| `MPOL_BIND` | Specified nodes only, no fallback | Latency-sensitive |
| `MPOL_PREFERRED` | Prefer node, allow fallback | Performance hint |
| `MPOL_INTERLEAVE` | Round-robin across nodes | Shared data |
| `MPOL_LOCAL` | Always local node | Per-CPU data |

| sysctl | Default | Effect |
|---|---|---|
| `kernel.numa_balancing` | 1 | Enable Auto-NUMA |
| `kernel.numa_balancing_scan_period_min_ms` | 1000 | Min scan period |
| `kernel.numa_balancing_scan_period_max_ms` | 60000 | Max scan period |
| `kernel.numa_balancing_scan_size_mb` | 256 | Pages scanned per period |
