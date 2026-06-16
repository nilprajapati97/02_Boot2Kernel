# `page_ext_init()` — Per-Page Extended Information

## Purpose

Allocates an array of `struct page_ext` entries — one per physical page — to store extended debugging/tracking metadata that is too large to fit in `struct page` itself. Used by features like KASAN, page owner tracking, and memory debugging.

## Source File

`mm/page_ext.c`

## Motivation

`struct page` is a fixed-size structure. Adding new debugging fields directly to it would increase its size for everyone, even production kernels that don't need them. `page_ext` is an optional, separately-allocated array that exists only when CONFIG options enable it.

## Structure

```c
struct page_ext {
    unsigned long flags;  // Bitmask of which extensions are active
    // Extension data follows immediately after (dynamic size)
};
```

The total size of each `page_ext` entry depends on which features are compiled in:

```
page_ext_size = sizeof(struct page_ext)
              + (CONFIG_PAGE_OWNER ? page_owner_ops.offset : 0)
              + (CONFIG_PAGE_POISONING ? page_poisoning_ops.offset : 0)
              + ...
```

## Features Using page_ext

| Feature | Kconfig | What it tracks |
|---------|---------|----------------|
| Page Owner | `CONFIG_PAGE_OWNER` | Stack trace of who allocated each page |
| Page Poisoning | `CONFIG_PAGE_POISONING` | Fill free pages with pattern to detect use-after-free |
| KASAN | `CONFIG_KASAN` | Memory access validity metadata |

## Early vs Late Initialization

There are two entry points:

1. **`page_ext_init_flatmem()`** — Called early (within `mm_core_init()`), before the buddy allocator. For FLATMEM memory models only. Uses `memblock`.

2. **`page_ext_init()`** — Called later (after `mem_init()`). For SPARSEMEM/VMEMMAP. Uses `vmalloc`.

## Memory Cost

For a system with 8GB RAM = 2,097,152 pages:
- If page_ext_size = 16 bytes: 32MB overhead
- If page_owner enabled (adds ~48 bytes): ~128MB overhead

This is why `CONFIG_PAGE_OWNER` is only for debug kernels.

## Cross-references

- [mm_core_init parent](../README.md)
- [mem_init](../mem_init/README.md)
