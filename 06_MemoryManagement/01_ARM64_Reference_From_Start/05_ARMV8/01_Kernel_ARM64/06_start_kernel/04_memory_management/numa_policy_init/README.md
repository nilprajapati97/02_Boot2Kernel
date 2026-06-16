# `numa_policy_init()` — Default NUMA Memory Policy

## Purpose

Initializes the default NUMA (Non-Uniform Memory Access) memory allocation policy. This establishes how the kernel and user processes will allocate memory across NUMA nodes when no explicit policy has been set.

## Source File

`mm/mempolicy.c`

```c
void __init numa_policy_init(void)
{
    nodemask_t interleave_nodes;
    unsigned long largest = 0;
    int nid, prefer = 0;

    // Find the node with the most memory (for default preferred node)
    for_each_node_state(nid, N_HIGH_MEMORY) {
        unsigned long total_pages = node_present_pages(nid);
        if (largest < total_pages) {
            largest = total_pages;
            prefer = nid;
        }
    }

    // Set default policy: MPOL_PREFERRED on largest node
    // (falls back to MPOL_INTERLEAVE for multi-node systems)
    set_mempolicy(MPOL_PREFERRED, ...);
    
    INIT_LIST_HEAD(&numa_inter_list);
}
```

## NUMA Policies

The Linux kernel supports five NUMA policies:

| Policy | Value | Behavior |
|--------|-------|----------|
| `MPOL_DEFAULT` | 0 | Use process's NUMA policy (usually local) |
| `MPOL_BIND` | 1 | Must allocate from specified nodes; OOM if impossible |
| `MPOL_INTERLEAVE` | 2 | Round-robin across nodes |
| `MPOL_PREFERRED` | 3 | Prefer a node; fall back to others |
| `MPOL_LOCAL` | 4 | Allocate from local (current) node |

## Default Policy for Kernel

For the kernel itself (before any process runs):
- **Single-node system**: `MPOL_LOCAL` (always allocate from node 0)
- **Multi-node system**: `MPOL_INTERLEAVE` over all nodes with RAM

The interleave policy spreads kernel allocations across nodes, which helps avoid hot spots during early init when many data structures are being allocated.

## Per-Process Policies

Once user processes start, they inherit `MPOL_DEFAULT` (which uses the process's VMA policy, falling back to `MPOL_LOCAL`). Processes can change their policy with `set_mempolicy()` syscall or `numactl`.

## VMA-Level Policy

Individual VMAs can have their own NUMA policy, overriding the process-level policy:

```c
struct vm_area_struct {
    /* ... */
    struct mempolicy *vm_policy;  // NULL = use process/system default
};
```

## NUMA Balancing

`CONFIG_NUMA_BALANCING` enables automatic NUMA page migration — the kernel monitors which CPU accesses which pages and migrates pages to be local to the accessing CPU. This is enabled/disabled via `/proc/sys/kernel/numa_balancing`.

## On Non-NUMA Systems

On UMA (single-node) systems, `numa_policy_init()` is a near no-op. `MPOL_LOCAL` always refers to node 0, the only node.

## Pre-conditions

- NUMA topology discovered (`setup_arch()`)
- `kmalloc()` available

## Post-conditions

- Default NUMA policy set
- `alloc_pages()` uses correct NUMA policy
- Processes inherit correct default policy

## Cross-references

- [Phase overview](../README.md)
