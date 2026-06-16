# `maple_tree_init()` — Maple Tree Slab Cache

## Purpose

Allocates the slab cache for `struct maple_node`, the internal node type of the maple tree. The maple tree replaced red-black trees for VMA (Virtual Memory Area) management in Linux 6.1.

## Source File

`lib/maple_tree.c`

```c
void __init maple_tree_init(void)
{
    maple_node_cache = kmem_cache_create("maple_node",
            sizeof(struct maple_node), sizeof(struct maple_node),
            SLAB_PANIC, NULL);
}
```

## What is a Maple Tree?

The maple tree is an RCU-safe B-tree variant optimized for storing non-overlapping ranges (like memory address ranges). It was designed specifically to replace the `rb_tree` + interval tree combination used for VMA management.

### Key Properties

| Property | Red-Black Tree | Maple Tree |
|----------|----------------|------------|
| Node size | ~48 bytes | 256 bytes (cache-aligned) |
| Branching factor | 2 | Up to 16 children |
| Range queries | O(log n) scan | O(log n) single lookup |
| RCU-safe | Requires rwlock | Built-in RCU support |
| Cache lines per traversal | ~O(log n) | O(log n) but fewer misses |

### Node Types

```c
// Dense (range) node: stores up to 16 pivots and 17 slots
struct maple_range_64 {
    struct maple_pnode *parent;
    unsigned long pivot[MAPLE_RANGE64_SLOTS - 1];  // 15 pivots
    union {
        void __rcu *slot[MAPLE_RANGE64_SLOTS];      // 16 slots
        struct {
            void __rcu *pad[MAPLE_RANGE64_SLOTS - 1];
            struct maple_metadata meta;
        };
    };
};

// Allocation (arange) node: similar but for gaps
```

## Primary Use: VMA Management

Every process has an `mm_struct` with a maple tree of VMAs:

```c
struct mm_struct {
    struct maple_tree mm_mt;  // All VMAs indexed by address range
    /* ... */
};
```

Finding the VMA for a given address:

```c
// Old (rb_tree):  
// traverse rb_tree, then check each VMA's vm_start/vm_end
vma = find_vma(mm, addr);  // O(log n) tree + O(1) check

// New (maple tree):
// Single range query: "find range covering addr"
vma = vma_lookup(mm, addr);  // O(log n) single lookup, better cache
```

## Maple Tree Operations

```c
// Insert a VMA [start, end) → vma:
mas_store(&mas, vma);

// Find VMA covering address:
vma = mas_find(&mas, addr);

// Walk all VMAs in range:
mas_for_each(&mas, vma, start, end) { ... }

// Check if address range is free:
mas_empty_area(&mas, start, end, size);
```

## RCU Concurrency

Maple tree supports RCU-protected reads:

```c
// Writer: holds mmap_write_lock
mas_store_gfp(&mas, vma, GFP_KERNEL);

// Reader: RCU read lock only (no mmap_read_lock needed for simple lookups)
rcu_read_lock();
vma = mas_find_rcu(&mas, addr);
rcu_read_unlock();
```

## Pre-conditions

- `kmem_cache_create()` available

## Post-conditions

- `maple_node_cache` allocated
- `mtree_store()`, `mtree_load()`, `mas_find()` functional

## Cross-references

- [Phase overview](../README.md)
- `radix_tree_init()`: [../radix_tree_init/README.md](../radix_tree_init/README.md)
- `anon_vma_init()`: [../../15_process_management/anon_vma_init/README.md](../../15_process_management/anon_vma_init/README.md)
