# `mem_init()` — Free memblock Memory to Buddy Allocator

## Purpose

The critical transition that frees all non-reserved physical memory pages from `memblock` management into the buddy allocator's free lists. After this point, `alloc_pages()` is fully operational.

## Source File

`arch/x86/mm/init_64.c` (arch-specific), calls `free_all_bootmem()` / `memblock_free_all()`

## The Transition in Detail

### Step 1: memblock State at Entry

At entry, `memblock` has two sets of regions:
- **Memory regions**: All physical RAM
- **Reserved regions**: bootmem, kernel image, initrd, per-CPU, etc.

```
Physical Memory Map (example):
[0x0000_0000 - 0x0009_FFFF]  640KB  conventional RAM    ← RESERVED (bootmem)
[0x000A_0000 - 0x000F_FFFF]  384KB  video/ROM           ← RESERVED
[0x0010_0000 - 0x0FFF_FFFF]  255MB  extended RAM        ← FREE (most)
[0x1000_0000 - 0x1500_0000]  80MB   kernel image        ← RESERVED
...
```

### Step 2: `memblock_free_all()`

```c
unsigned long __init memblock_free_all(void)
{
    unsigned long pages;
    
    reset_all_zones_managed_pages();
    
    // Walk all memory regions, skip reserved ones
    // For each free page: call __free_pages_bootmem()
    pages = free_low_memory_core_early();
    
    totalram_pages_add(pages);
    return pages;
}
```

### Step 3: Adding Pages to Buddy Allocator

For each free physical page, `__free_pages_bootmem()` calls `__free_pages()` which inserts the page into the buddy allocator's free list at the appropriate order.

The buddy allocator then coalesces adjacent free pages into higher-order blocks:

```
Page 0 free → free_area[0]
Page 1 free → buddy of page 0 → coalesce → free_area[1] (2-page block)
Page 2 free → free_area[0]
Page 3 free → buddy of page 2 → coalesce → free_area[1] → buddy of 0-1 → free_area[2]
```

## Memory Regions Not Freed

These regions stay reserved and are **not** freed:

| Region | Why Kept |
|--------|----------|
| Kernel `.text`, `.data`, `.bss` | Kernel code/data |
| Per-CPU areas | Per-CPU variable storage |
| Page tables (early) | Need for VA→PA mapping |
| initrd | Root filesystem |
| Reserved BIOS areas | Hardware needs them |
| ACPI tables | Needed after init |

## Post-conditions

- `totalram_pages` set to actual available pages
- Buddy allocator free lists populated
- `alloc_pages(GFP_KERNEL, order)` fully functional
- `__get_free_pages()` functional
- memblock data structures still exist (for reserved region queries)

## Memory Info Printed

After `mem_init()`, the kernel prints:

```
Memory: 7823456K/8388608K available (14338K kernel code, 2572K rwdata,
        4964K rodata, 1480K init, 2968K bss, 565152K reserved,
        0K cma-reserved)
```

## Cross-references

- [mm_core_init parent](../README.md)
- [kmem_cache_init](../kmem_cache_init/README.md) — runs after this, needs buddy allocator
