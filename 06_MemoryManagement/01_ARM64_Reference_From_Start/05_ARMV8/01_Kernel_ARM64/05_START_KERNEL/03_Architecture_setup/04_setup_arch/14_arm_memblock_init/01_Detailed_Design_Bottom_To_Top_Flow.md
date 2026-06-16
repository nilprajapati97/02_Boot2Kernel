# arm_memblock_init() — Detailed Design

## 1. Position in setup_arch() Boot Sequence

```
setup_arch()
  ├── adjust_lowmem_bounds()      ← arm_lowmem_limit established
  └── arm_memblock_init(mdesc)    ← *** THIS FUNCTION ***
        │ Reserves all non-general-purpose memory in memblock
        │ After this, memblock reflects only "usable by kernel" RAM
        ├── memblock_reserve(KERNEL_START, kernel_size)
        ├── reserve_initrd_mem()
        ├── arm_mm_memblock_reserve()    (page table memory)
        ├── mdesc->reserve()             (board-specific)
        ├── early_init_fdt_scan_reserved_mem()
        ├── dma_contiguous_reserve(arm_dma_limit)
        └── arm_memblock_steal_permitted = false
```

`arm_memblock_init()` is the **reservation phase** of early memory management. Everything that must be kept safe from the kernel's general allocator is reserved here, before `paging_init()` builds the page tables and before the buddy allocator is initialized.

---

## 2. Source Code Analysis

**File:** `arch/arm/mm/init.c`

```c
void __init arm_memblock_init(const struct machine_desc *mdesc)
{
    /* Reserve the kernel text, init, data, bss */
    memblock_reserve(__pa(KERNEL_START), KERNEL_END - KERNEL_START);
```

`KERNEL_START` is `_text` (start of kernel code). `KERNEL_END` is `_end` (end of BSS). This prevents memblock from ever allocating memory that overlaps the kernel itself.

```c
    /* Reserve physical memory for initrd */
    reserve_initrd_mem();
```

If an initrd was passed by the bootloader (phys_initrd_start/size from FDT or ATAGs), this call reserves that physical region in memblock. The initrd is later mounted as rootfs.

```c
    /* Reserve memory for early page tables */
    arm_mm_memblock_reserve();
```

ARM32's page table setup allocates physical pages for the initial page tables during `paging_init()`. These allocations are done with `memblock_alloc()` but the exact locations depend on alignment. This function pre-reserves the expected range.

```c
    /* Board-specific reservations */
    if (mdesc->reserve)
        mdesc->reserve();
```

This is the most architecturally significant call. The machine descriptor's `reserve()` callback lets board code reserve hardware-specific memory:
- **GPU framebuffer**: Some SoCs have a fixed framebuffer region (e.g., `memblock_reserve(0x1E000000, 0x02000000)` for 32MB GPU memory)
- **Shared memory with firmware**: TrustZone secure world or DSP firmware regions
- **Boot splash**: Some systems keep a framebuffer reservation for the bootlogo
- **DMA-capable device buffers**: Pre-allocated buffers for devices that cannot use CMA

After `mdesc->reserve()`, memblock may have significantly fewer available regions. This is why `adjust_lowmem_bounds()` must be called AGAIN after `arm_memblock_init()`.

```c
    /* Process /reserved-memory nodes from Device Tree */
    early_init_fdt_scan_reserved_mem();
```

Modern boards use FDT `/reserved-memory` nodes instead of (or in addition to) `mdesc->reserve()`:

```
/ {
    reserved-memory {
        #address-cells = <1>;
        #size-cells = <1>;
        ranges;

        gpu_reserved: gpu@1e000000 {
            reg = <0x1e000000 0x2000000>;  /* 32MB GPU memory */
            no-map;
        };

        cma_pool: linux,cma {
            size = <0x8000000>;  /* 128MB CMA */
            alignment = <0x2000000>;
            linux,cma-default;
        };
    };
};
```

`early_init_fdt_scan_reserved_mem()` processes each node:
- `no-map`: → `memblock_mark_nomap()` (keep from being used OR mapped)
- Without `no-map`: → `memblock_reserve()` (keep but still mapped)
- `linux,cma-default`: → sets up the default CMA pool

```c
    dma_contiguous_reserve(arm_dma_limit);
```

Allocates the CMA (Contiguous Memory Allocator) pool. CMA reserves a large contiguous region for devices that need physically contiguous DMA buffers (camera, GPU, video codec). This uses the `arm_dma_limit` set by `setup_dma_zone()` to constrain the CMA pool within DMA-addressable physical memory.

Typical CMA reserve:
```
dma_contiguous_reserve(0xFFFFFFFF)  ← all physical memory DMA-addressable
  → cma_init_reserved_mem() reserves, e.g., 64MB at highest available PA
  → memblock_reserve(cma_base, cma_size)
  → this region is initially marked reserved; freed to CMA allocator later
```

```c
    arm_memblock_steal_permitted = false;
}
```

**Critical locking point**: After `arm_memblock_init()`, no more "steal" allocations (permanent memblock allocations that bypass normal allocation) are allowed. This flag prevents code called after this point from using `memblock_alloc_range()` in ways that bypass the reserved regions.

---

## 3. Memory Reservation Types

| Reservation | Function | Purpose |
|-------------|----------|---------|
| Kernel image | `memblock_reserve(KERNEL_START, ...)` | Protect kernel code/data/bss |
| initrd | `reserve_initrd_mem()` | Preserve initial ramdisk |
| Page tables | `arm_mm_memblock_reserve()` | Space for initial MMU tables |
| Board-specific | `mdesc->reserve()` | GPU, firmware, DMA buffers |
| FDT /reserved-memory | `early_init_fdt_scan_reserved_mem()` | Modern DT-based reservations |
| CMA pool | `dma_contiguous_reserve()` | Contiguous DMA buffer pool |

---

## 4. Call Tree (Bottom-Up)

```
memblock_reserve()                  ← mm/memblock.c
memblock_mark_nomap()               ← mm/memblock.c
memblock_alloc_range_nid()          ← mm/memblock.c (for CMA)
        ▲
reserve_initrd_mem()                ← arch/arm/mm/init.c
arm_mm_memblock_reserve()           ← arch/arm/mm/mmu.c
early_init_fdt_scan_reserved_mem()  ← drivers/of/fdt.c
dma_contiguous_reserve()            ← kernel/dma/contiguous.c
        ▲
arm_memblock_init(mdesc)            ← arch/arm/mm/init.c:183
        ▲
setup_arch()                        ← arch/arm/kernel/setup.c
```

---

## 5. The arm_memblock_steal_permitted Flag

```c
/* arch/arm/mm/mmu.c */
int arm_memblock_steal_permitted = 1;

/* arch/arm/mm/init.c */
phys_addr_t __init arm_memblock_steal(phys_addr_t size, phys_addr_t align)
{
    phys_addr_t phys;

    BUG_ON(!arm_memblock_steal_permitted);

    phys = memblock_phys_alloc(size, align);
    if (!phys)
        panic("Failed to steal %pa bytes at %pS\n",
              &size, (void *)_RET_IP_);

    memblock_free(phys, size);
    memblock_remove(phys, size);
    return phys;
}
```

`arm_memblock_steal()` removes memory from memblock entirely — it can never be reclaimed. Used for early data structures that must persist forever (e.g., initial page tables). Setting `arm_memblock_steal_permitted = false` at the end of `arm_memblock_init()` enforces that no more stealing happens after the reservation phase.

---

## 6. Effect on Memory Zones

After `arm_memblock_init()`, the memblock state determines:
- **Total RAM**: sum of all memblock memory regions
- **Reserved RAM**: sum of all reserved entries
- **Available for buddy**: Total - Reserved
- **CMA region**: Marked reserved in memblock, but freed to CMA allocator later

These numbers appear in `/proc/meminfo`:
```
MemTotal:         <available_for_buddy>
...
CmaTotal:         <cma_size>
```

---

## 7. Interview Q&A

**Q1: What is CMA and why is it reserved in arm_memblock_init()?**
> CMA (Contiguous Memory Allocator) provides physically contiguous memory to devices that need it (camera, GPU, video decoder). It reserves a large contiguous region early in boot when memory is unfragmented. This reserved region is initially unavailable to the buddy allocator. When a device needs a contiguous buffer, CMA migrates any moveable pages out of its region and returns it. After device release, those pages become moveable again. The key insight: CMA must be reserved before any allocation, because once even one page in a range is allocated, the range is no longer contiguous.

**Q2: Why is arm_memblock_steal_permitted set to false at the end of arm_memblock_init()?**
> Stealing memory (permanently removing it from memblock) is only safe before the reservation phase is complete. After `arm_memblock_init()`, the memory map is finalized. If stealing were allowed afterward, code could remove physical memory that was properly accounted for by `paging_init()` or zone setup — creating a mismatch between what's mapped and what the allocator knows about. Setting this flag prevents accidental misuse of `arm_memblock_steal()` in later initialization code.

**Q3: What happens if mdesc->reserve() removes memory that was already counted as lowmem?**
> If `mdesc->reserve()` reserves physical memory near the top of what was computed as lowmem (e.g., reserves the last 128MB of a 1GB region), `arm_lowmem_limit` computed by the first `adjust_lowmem_bounds()` call is now wrong. The "gap" in memblock at the top of what was lowmem means `paging_init()` would map a VA range to physical memory that's now reserved for hardware. This is exactly why the second `adjust_lowmem_bounds()` call exists — it recalculates after all reservations are in place.

**Q4: Why must the kernel image be reserved explicitly? Isn't it obviously occupied?**
> The kernel is loaded by the bootloader to a physical address. After handoff, the bootloader is gone and there's no hardware mechanism preventing the kernel from allocating its own physical memory. `memblock_reserve(KERNEL_START, ...)` is the explicit software reservation that protects kernel memory from the kernel's own memory allocator. Without this, an early `kmalloc()` that happens to return a physical address within the kernel text would corrupt running code — a catastrophic bug.

**Q5: What is the relationship between /reserved-memory FDT nodes and memblock?**
> FDT `/reserved-memory` nodes are the standard DT mechanism for expressing memory reservations. `early_init_fdt_scan_reserved_mem()` translates each node into memblock calls: nodes with `no-map` property call `memblock_mark_nomap()` (hardware-owned regions that must not be mapped); nodes without `no-map` call `memblock_reserve()` (reserved but still visible to kernel). Nodes with `linux,cma-default` trigger CMA pool initialization. This allows board-specific reservations to be expressed in the DTB rather than in C code, making the kernel image more generic.
