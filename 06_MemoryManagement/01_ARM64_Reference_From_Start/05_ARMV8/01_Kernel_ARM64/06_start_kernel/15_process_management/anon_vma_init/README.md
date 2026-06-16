# `anon_vma_init()` — Anonymous VMA Slab Cache

## Purpose

Creates the slab cache for `struct anon_vma` and `struct anon_vma_chain`, which are used by the reverse mapping (rmap) infrastructure to track which page table entries map each anonymous page.

## Source File

`mm/rmap.c`

```c
void __init anon_vma_init(void)
{
    anon_vma_cachep = kmem_cache_create("anon_vma",
            sizeof(struct anon_vma),
            0,
            SLAB_TYPESAFE_BY_RCU | SLAB_PANIC | SLAB_ACCOUNT,
            anon_vma_ctor);
    
    anon_vma_chain_cachep = KMEM_CACHE(anon_vma_chain,
            SLAB_PANIC | SLAB_ACCOUNT);
}
```

## The Reverse Mapping Problem

The kernel needs to find all page table entries (PTEs) mapping a given physical page. This is needed for:
- **Page reclaim (swapping)**: Must unmap the page from all processes before evicting
- **Copy-on-Write**: When a forked process writes to a shared page, find the original mapping
- **NUMA balancing**: Migrate pages to the NUMA node where they are most accessed

For file-backed pages, the inode's `i_mapping` (address_space) points to all mapped PTEs. For anonymous pages (heap, stack, `mmap(MAP_ANONYMOUS)`), there is no file — this is what `anon_vma` handles.

## Data Structure

```
struct mm_struct                struct anon_vma
    └── struct vm_area_struct      ├── struct rb_root rb_root
          │                        │     (all VMAs sharing this anon_vma)
          └── struct anon_vma_chain ←──→ struct anon_vma_chain
                │                           │
                └── struct anon_vma ←───────┘
                      └── ... more chains
```

### `struct anon_vma`

```c
struct anon_vma {
    struct anon_vma    *root;       // Root of inheritance tree
    struct rw_semaphore rwsem;      // Protects rb_root
    atomic_t            refcount;
    unsigned            degree;
    struct anon_vma    *parent;     // Parent in fork tree
    struct rb_root      rb_root;    // All anon_vma_chain entries
};
```

### Fork and anon_vma Inheritance

```
Before fork():
    Parent process VMA ────────→ anon_vma_A

After fork():
    Parent VMA ─────────────────→ anon_vma_A
    Child VMA ──→ anon_vma_B ──→ anon_vma_A
```

When the parent writes to a CoW page:
1. Kernel needs to find the child's PTE for the same physical page
2. Walks `anon_vma_A`'s chain to find all VMAs (parent + child)
3. Unshares the page for the parent

## `SLAB_TYPESAFE_BY_RCU`

The `anon_vma` slab uses `SLAB_TYPESAFE_BY_RCU` — freed objects are not returned to the OS immediately. This allows RCU-protected code to safely access `anon_vma` pointers found in page flags without taking locks, as the object type doesn't change even after freeing.

## Cross-references

- [Phase overview](../README.md)
- `mm_core_init()`: [../../04_memory_management/mm_core_init/README.md](../../04_memory_management/mm_core_init/README.md)
- `maple_tree_init()`: [../../07_data_structures/maple_tree_init/README.md](../../07_data_structures/maple_tree_init/README.md) — VMAs stored in maple tree
