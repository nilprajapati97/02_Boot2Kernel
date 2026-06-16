# Q24 — Design a NUMA-Aware Resource Allocator

---

## 1. Problem Statement

On multi-socket systems (NUMA), memory access latency is non-uniform: accessing DRAM on the local NUMA node costs ~80 ns, while accessing a remote node costs ~140–160 ns (70–100% more). For a GPU driver managing buffers for compute kernels, or a kernel allocator managing slab caches, placing memory on the wrong NUMA node causes consistent bandwidth degradation and latency jitter.

Design a NUMA-aware resource allocator that ensures memory allocations, CPU execution, and device DMA all occur on the same NUMA node.

---

## 2. Requirements

### 2.1 Functional Requirements
- Allocate memory on the NUMA node local to the requesting CPU.
- Pin threads to CPUs within the same NUMA node as their memory.
- Configure GPU/PCIe device DMA to use NUMA-local memory.
- Support NUMA-aware slab caches (one per node).
- Handle NUMA imbalance: migrate memory and threads when topology changes.

### 2.2 Non-Functional Requirements
- Memory allocation NUMA locality: > 95% of allocations on local node.
- Remote NUMA access rate: < 5% of total memory accesses.
- NUMA-aware allocation overhead vs non-NUMA: < 10%.

---

## 3. Constraints & Assumptions

- 2-socket Intel Xeon server (NUMA node 0: CPUs 0–31, NUMA node 1: CPUs 32–63).
- GPU (PCIe device) connected to NUMA node 0's root complex.
- Linux NUMA infrastructure: `libnuma`, `set_mempolicy()`, `mbind()`.
- Kernel: NUMA-aware page allocator (zone lists), NUMA-aware SLUB.

---

## 4. Architecture Overview

```
  NUMA Node 0                         NUMA Node 1
  ┌──────────────────────┐           ┌──────────────────────┐
  │  CPUs 0–31           │           │  CPUs 32–63          │
  │  DRAM: 256 GB        │◄─────────►│  DRAM: 256 GB        │
  │  PCIe Root Complex   │  QPI/UPI  │                      │
  │    └── GPU (BDF 03)  │           │                      │
  └──────────────────────┘           └──────────────────────┘

  For GPU workload (DMA to system RAM):
  ┌────────────────────────────────────────────────────────┐
  │  GPU DMA → PCIe Root Complex (NUMA node 0)            │
  │  → DRAM on NUMA node 0 (local, fast: 32 GB/s)         │
  │  vs                                                    │
  │  GPU DMA → cross QPI → DRAM on NUMA node 1 (slow: 16 GB/s) │
  └────────────────────────────────────────────────────────┘

  Allocator Strategy:
  1. Identify NUMA node of current CPU → allocate on same node
  2. Identify NUMA node of PCIe device → allocate DMA buffers on same node
  3. Bind threads to CPUs on same node as their data
```

---

## 5. Core Data Structures

### 5.1 NUMA Node Descriptor

```c
struct pglist_data {  /* pg_data_t */
    struct zone         node_zones[MAX_NR_ZONES]; /* ZONE_DMA, ZONE_NORMAL, ZONE_MOVABLE */
    struct zonelist     node_zonelists[MAX_ZONELISTS]; /* fallback order for allocations */
    int                 nr_zones;
    int                 node_id;             /* NUMA node number */
    unsigned long       node_start_pfn;      /* first PFN on this node */
    unsigned long       node_present_pages;
    unsigned long       node_spanned_pages;
    struct per_cpu_pageset *per_cpu_pageset; /* per-CPU page cache for hot pages */
};
```

### 5.2 Memory Policy (per-task)

```c
struct mempolicy {
    atomic_t            refcnt;
    unsigned short      mode;        /* MPOL_PREFERRED, MPOL_BIND, MPOL_INTERLEAVE */
    unsigned short      flags;
    nodemask_t          nodes;       /* allowed nodes */
    union {
        nodemask_t  preferred_node;  /* MPOL_PREFERRED: prefer this node */
        nodemask_t  bind_nodes;      /* MPOL_BIND: allocate ONLY from these nodes */
    };
};

/* Set via: */
/* mbind(addr, len, MPOL_BIND, &nodemask, ...) */
/* set_mempolicy(MPOL_PREFERRED, &nodemask, ...) */
/* numa_alloc_onnode(size, node) → libnuma wrapper */
```

### 5.3 NUMA-Aware Slab Cache

```c
struct kmem_cache {
    struct kmem_cache_cpu  __percpu *cpu_slab;  /* per-CPU fast path */
    struct kmem_cache_node *node[MAX_NUMNODES]; /* per-NUMA-node partial slabs */
    /* ... */
};

struct kmem_cache_node {
    spinlock_t       list_lock;
    unsigned long    nr_partial;
    struct list_head partial;       /* partially filled slabs on this node */
    /* Allocation: prefer node-local partial before going to buddy allocator */
};

/* NUMA-local slab allocation: */
/* kmem_cache_alloc_node(cache, GFP_KERNEL, numa_node_id()) */
/* → prefers partial slabs already on current node */
/* → falls back to new page from local node's buddy allocator */
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Page Allocator NUMA Policy

```c
/* alloc_pages_node(nid, gfp, order): explicit NUMA node allocation */
struct page *page = alloc_pages_node(0, GFP_KERNEL, 0);
/* nid=0: allocate from NUMA node 0's ZONE_NORMAL */
/* Fallback: if node 0 is full → node 1 (zonelist fallback order) */

/* GFP_THISNODE: allocate on current node only, fail if full */
page = alloc_pages(GFP_KERNEL | __GFP_THISNODE, 0);

/* Default (no explicit node): */
page = alloc_page(GFP_KERNEL);
/* → uses current task's mempolicy → prefers numa_node_id() */
```

### 6.2 NUMA Topology Discovery

```c
/* At boot: kernel reads ACPI SRAT (System Resource Affinity Table) */
/* SRAT: maps physical address ranges → NUMA proximity domains */
/* SLIT (System Locality Information Table): hop distances between nodes */

/* Runtime queries: */
int node = numa_node_id();                    /* current CPU's NUMA node */
int node = cpu_to_node(cpu);                  /* CPU's NUMA node */
int node = pci_dev_to_node(pdev);             /* PCIe device's NUMA node */
const struct cpumask *cpus = cpumask_of_node(node); /* CPUs on a node */

/* PCIe NUMA locality: */
/* pci_dev->dev.numa_node: set by firmware (ACPI _PXM or PCIe ACPI affinity) */
/* For GPU: always allocate DMA buffers on same node as GPU */
int gpu_node = dev_to_node(&pdev->dev);
buf = dma_alloc_coherent_node(&pdev->dev, size, &dma_handle, GFP_KERNEL, gpu_node);
```

### 6.3 NUMA Balancing — Automatic Migration

Linux NUMA balancing (CONFIG_NUMA_BALANCING) automatically migrates pages to the node where they are most accessed:

```
Mechanism:
1. Periodically (numa_scan_period): mark process pages as inaccessible (prot_none).
2. When process accesses a prot_none page: take page fault.
3. Fault handler: record which NUMA node took the fault.
4. If page is on remote node AND fault is on local node:
       migrate_pages(page, local_node)  /* move page closer to CPU */
5. Also: task migration — move task to node where its pages are resident.

Heuristics:
    numa_faults[cpu][node]: counter per (CPU, memory node) pair
    task_numa_placement() → choose best node for task based on fault history
```

**Disable for RT or GPU tasks** (migration adds latency spikes):
```c
prctl(PR_SET_NUMA_BALANCING, 0, 0, 0, 0);  /* disable for this task */
/* Or boot: numa_balancing=0 */
```

### 6.4 NUMA-Aware Thread Pool

```c
struct numa_thread_pool {
    int           num_nodes;
    /* One worker thread group per NUMA node */
    struct worker_group {
        cpu_set_t   cpumask;        /* CPUs on this node */
        int         node_id;
        pthread_t   workers[MAX_WORKERS_PER_NODE];
        struct work_queue queue;    /* lock-free queue for this node */
    } nodes[MAX_NUMNODES];
};

/* Dispatch: assign work to thread on same NUMA node as input data */
void submit_work(struct numa_thread_pool *pool, struct work_item *item) {
    int data_node = page_to_nid(virt_to_page(item->data));
    struct worker_group *grp = &pool->nodes[data_node];
    enqueue(&grp->queue, item);     /* worker on same node picks it up */
}
```

### 6.5 mbind — Per-VMA Memory Policy

```c
/* For large buffer allocations (ML model weights): */
/* Pin the memory to NUMA node 0 where GPU is connected */

void *model_weights = mmap(NULL, SIZE_2GB, PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

unsigned long nodemask = 1UL << 0;  /* node 0 only */
mbind(model_weights, SIZE_2GB,
      MPOL_BIND,
      &nodemask, sizeof(nodemask)*8,
      MPOL_MF_MOVE);  /* also move existing pages if already allocated */

/* MPOL_BIND: all page faults in this VMA → allocate on node 0 */
/* MPOL_MF_MOVE: migrate any existing pages on node 1 → node 0 */
```

### 6.6 NUMA Statistics and Tuning

```bash
numastat -p <pid>         # show per-node memory allocation for process
numactl --show            # current NUMA policy
numactl --hardware        # topology: nodes, distances, CPUs
numactl --membind=0 --cpunodebind=0 ./myprogram  # run with node 0 binding

# NUMA distance table (from SLIT):
# Node 0 → 0: 10 (local), 0 → 1: 21 (remote × 2.1x latency)
```

---

## 7. Trade-off Analysis

| Policy | Locality | Fragmentation | Latency Variance |
|---|---|---|---|
| MPOL_PREFERRED | Best-effort local | Low | Low (local usually) |
| MPOL_BIND | Strict local | Higher (single node) | None (always local) |
| MPOL_INTERLEAVE | Distributed | None | High (round-robin) |
| Default (no policy) | Depends on allocating CPU | Low | High (task migration) |
| NUMA balancing ON | Auto-migrated | Low | Spiky (migration stalls) |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| NUMA page alloc | `mm/page_alloc.c` | `alloc_pages_node()`, `alloc_pages_nopolicy()` |
| Memory policy | `mm/mempolicy.c` | `mbind()`, `set_mempolicy()`, `mpol_misplaced()` |
| NUMA balancing | `kernel/sched/fair.c` | `task_numa_fault()`, `task_numa_placement()` |
| SLUB NUMA node | `mm/slub.c` | `kmem_cache_alloc_node()`, `get_partial_node()` |
| PCIe NUMA node | `drivers/pci/pci.c` | `pci_dev_to_node()`, `set_dev_node()` |
| numastat kernel | `mm/vmstat.c` | `numa_stat_name[]` |
| CPU/node mapping | `include/linux/topology.h` | `cpu_to_node()`, `cpumask_of_node()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Remote NUMA Accesses Dominate
```bash
numastat -p <pid>    # shows numa_miss (allocated on non-preferred node)
# High numa_miss: allocation fell back to remote node due to local OOM
# Fix: reduce local node memory pressure, use MPOL_BIND (fail rather than remote alloc)
```

### 9.2 NUMA Balancing Migration Latency
```bash
perf stat -e 'numa:*' -p <pid> sleep 10
# High numa:numa_pages_migrated: NUMA balancing is migrating pages (causes stalls)
# Fix for GPU/RT workloads: prctl(PR_SET_NUMA_BALANCING, 0)
```

### 9.3 GPU DMA to Wrong NUMA Node
```bash
# Check GPU PCIe NUMA locality:
cat /sys/bus/pci/devices/0000:03:00.0/numa_node
# Should be 0 if GPU is on node 0's root complex
# If -1: firmware didn't set affinity → set manually:
echo 0 > /sys/bus/pci/devices/0000:03:00.0/numa_node
```

---

## 10. Performance Considerations

- **First-touch policy (default):** Pages are allocated on the node of the first CPU to touch them. If the main thread (on node 0) initializes all arrays, all memory is on node 0 even if worker threads on node 1 use it. Solution: use first-touch from worker threads (parallel init).
- **Huge pages and NUMA:** 2MB huge pages are allocated from a specific NUMA node's HugeTLB pool. Ensure pool is populated on the correct node: `echo 512 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages`.
- **NUMA-aware allocator benchmark:** `stream` benchmark with/without `numactl --membind` shows 30–50% bandwidth difference on 2-socket systems.
- **Per-CPU allocator:** `kmem_cache_alloc()` uses per-CPU slab — almost always NUMA-local since per-CPU data is on the local node.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. NUMA latency numbers: local ~80 ns, remote ~140–160 ns, 70–100% more — cite specific numbers.
2. `alloc_pages_node()` vs `alloc_pages()` — explicit node vs mempolicy-governed.
3. `mbind(MPOL_BIND)` for large buffers: strict binding fails rather than remote alloc.
4. PCIe NUMA locality: `pci_dev_to_node()` — GPU DMA buffers must be on GPU's NUMA node.
5. NUMA balancing: automatic but can cause latency spikes — disable for GPU/RT workloads.
6. First-touch policy: parallel data initialization ensures data is local to worker threads.
7. SLUB NUMA-per-node: each `kmem_cache_node` has its own partial list per NUMA node.
8. `numastat -p`: primary diagnostic tool for NUMA allocation quality.
