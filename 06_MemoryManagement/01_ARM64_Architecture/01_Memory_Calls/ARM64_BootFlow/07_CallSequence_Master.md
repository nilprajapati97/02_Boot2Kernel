# ARMv8 Linux Memory Subsystem — Full Call Sequence

## End-to-End Boot Call Chain

```mermaid
sequenceDiagram
    participant CPU as CPU (reset)
    participant ASM as head.S (ASM)
    participant C as start_kernel (C)
    participant ARCH as setup_arch
    participant MEMBLOCK as memblock
    participant MMU as paging_init
    participant BUDDY as bootmem_init
    participant SLAB as kmem_cache_init

    CPU->>ASM: Reset, jump to _head
    ASM->>ASM: primary_entry
    ASM->>ASM: __create_page_tables
    ASM->>ASM: __primary_switch
    ASM->>C: __primary_switched
    C->>ARCH: start_kernel
    ARCH->>ARCH: setup_arch
    ARCH->>MEMBLOCK: early_fixmap_init
    ARCH->>MEMBLOCK: arm64_memblock_init
    MEMBLOCK->>MEMBLOCK: memblock_add, memblock_reserve
    ARCH->>MMU: paging_init
    MMU->>MMU: map_mem, __map_memblock
    ARCH->>BUDDY: bootmem_init
    BUDDY->>BUDDY: zone_sizes_init, free_area_init
    BUDDY->>BUDDY: memblock_free_all
    ARCH->>SLAB: kmem_cache_init
    SLAB->>SLAB: kmalloc caches up
```

## Resource Timeline

| Function | Allocator Available |
|----------|--------------------|
| head.S   | Static .bss        |
| setup_arch | memblock         |
| paging_init | memblock        |
| bootmem_init | buddy          |
| kmem_cache_init | slab        |

## File References
- arch/arm64/kernel/head.S
- init/main.c
- arch/arm64/kernel/setup.c
- arch/arm64/mm/init.c
- arch/arm64/mm/mmu.c
- mm/memblock.c
- mm/page_alloc.c
- mm/slub.c
