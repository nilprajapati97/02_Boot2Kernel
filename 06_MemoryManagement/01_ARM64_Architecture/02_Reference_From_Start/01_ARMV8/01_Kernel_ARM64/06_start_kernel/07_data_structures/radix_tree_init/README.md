# `radix_tree_init()` — Radix Tree / XArray Slab Cache

## Purpose

Allocates the slab cache for `struct radix_tree_node` (and the equivalent `xa_node` for XArray). This enables efficient sparse indexed lookups used by the page cache, IDR, and many other subsystems.

## Source File

`lib/radix-tree.c`

```c
void __init radix_tree_init(void)
{
    int ret;

    BUILD_BUG_ON(RADIX_TREE_MAX_TAGS + __GFP_BITS_SHIFT > 32);
    BUILD_BUG_ON(ROOT_IS_IDR & ~ROOT_FLAGS);
    
    radix_tree_node_cachep = kmem_cache_create("radix_tree_node",
                sizeof(struct radix_tree_node), 0,
                SLAB_PANIC | SLAB_RECLAIM_ACCOUNT,
                radix_tree_node_ctor);
    
    ret = cpuhp_setup_state_nocalls(CPUHP_RADIX_DEAD,
                "lib/radix:dead", NULL, radix_tree_cpu_dead);
    WARN_ON(ret < 0);
}
```

## Radix Tree Structure

A radix tree (also known as a trie) is a tree indexed by integer keys. The Linux implementation is a **multi-level array tree** with variable height:

```
Key = 0x12345678 (32-bit page index)

Level 0 root (6-bit slots = 64 children):
    index = key >> (RADIX_TREE_MAP_SHIFT * height)
    
Level 1 internal node:
    index = (key >> (RADIX_TREE_MAP_SHIFT * (height-1))) & MASK
    
Level 2 leaf node:
    → stores the actual data pointer (e.g., struct page *)
```

Default `RADIX_TREE_MAP_SHIFT = 6` → 64 children per node.

## XArray: The Modern Replacement

Since Linux 5.0, most code uses **XArray** (eXtensible Array), which is built on the same underlying radix tree:

```c
// Old API:
struct radix_tree_root tree;
radix_tree_insert(&tree, index, item);
item = radix_tree_lookup(&tree, index);

// New XArray API:
DEFINE_XARRAY(xa);
xa_store(&xa, index, item, GFP_KERNEL);
item = xa_load(&xa, index);
```

XArray provides a cleaner API with better locking semantics.

## Key User: Page Cache

The page cache uses a radix/XArray tree per file to map page offsets to `struct page *`:

```c
struct address_space {
    struct inode        *host;
    struct xarray        i_pages;   // Maps offset → struct page*
    struct rw_semaphore  invalidate_lock;
    gfp_t               gfp_mask;
    /* ... */
};

// Lookup: "Give me page at offset 42 of this file"
page = xa_load(&file->f_mapping->i_pages, 42);
```

## Key User: IDR (ID Allocator)

IDR maps small integers (IDs) to pointers:

```c
// Process table: PID → task_struct
idr_find(&task_struct_table, pid);

// File descriptor table: fd → file
// (actually uses xarray directly now)
```

## Tagging

Radix tree nodes support per-entry tags (bit flags):

```c
// Tags used by page cache:
PAGECACHE_TAG_DIRTY   // Page is dirty (needs writeback)
PAGECACHE_TAG_WRITEBACK // Page is being written back
PAGECACHE_TAG_TOWRITE // Page marked for writeback
```

This allows efficient "find all dirty pages in range" queries.

## Pre-conditions

- `kmem_cache_create()` available (slab allocator ready)

## Post-conditions

- `radix_tree_node_cachep` allocated
- `radix_tree_insert()`, `radix_tree_lookup()`, `xa_store()`, `xa_load()` functional

## Cross-references

- [Phase overview](../README.md)
- `maple_tree_init()`: [../maple_tree_init/README.md](../maple_tree_init/README.md)
