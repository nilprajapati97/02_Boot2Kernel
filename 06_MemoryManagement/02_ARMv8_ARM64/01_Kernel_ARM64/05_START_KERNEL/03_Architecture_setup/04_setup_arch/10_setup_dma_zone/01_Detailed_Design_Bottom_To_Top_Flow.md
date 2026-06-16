# setup_dma_zone() — Detailed Design: Bottom-to-Top Flow

## 1. Position in setup_arch() Boot Sequence

```
setup_arch()
  ├── early_mm_init(mdesc)        ← memory type table built
  └── setup_dma_zone(mdesc)       ← *** THIS FUNCTION *** (line 1151)
        └── sets arm_dma_limit
        └── sets arm_dma_pfn_limit
```

`setup_dma_zone()` establishes the **DMA zone boundary** — the highest physical address that DMA-capable devices can address. This boundary is used later to define the `ZONE_DMA` memory zone, ensuring DMA allocations never return memory beyond the hardware's addressing capability.

---

## 2. Source Code

**File:** `arch/arm/mm/init.c`

```c
void __init setup_dma_zone(const struct machine_desc *mdesc)
{
#ifdef CONFIG_ZONE_DMA
    if (mdesc->dma_zone_size) {
        arm_dma_zone_size = mdesc->dma_zone_size;
        arm_dma_limit = PHYS_OFFSET + arm_dma_zone_size - 1;
    } else
        arm_dma_limit = 0xffffffff;
    arm_dma_pfn_limit = arm_dma_limit >> PAGE_SHIFT;
#endif
}
```

**Global variables set:**

```c
phys_addr_t arm_dma_limit;        /* highest DMA-addressable physical address */
unsigned long arm_dma_pfn_limit;  /* arm_dma_limit >> PAGE_SHIFT (in page frames) */
```

---

## 3. Key Logic Walkthrough

### Case 1: Machine descriptor has `dma_zone_size` set

```c
arm_dma_zone_size = mdesc->dma_zone_size;
arm_dma_limit = PHYS_OFFSET + arm_dma_zone_size - 1;
```

Example: Raspberry Pi 1 (BCM2835) has:
```c
.dma_zone_size = SZ_256M,   /* 256 MB DMA zone */
```

With `PHYS_OFFSET = 0x00000000`:
- `arm_dma_limit = 0x00000000 + 0x10000000 - 1 = 0x0FFFFFFF`
- DMA zone covers physical 0x00000000 – 0x0FFFFFFF

Any DMA allocation (`GFP_DMA`) will return memory within this range.

### Case 2: No `dma_zone_size` — default full 32-bit range

```c
arm_dma_limit = 0xffffffff;
```

This means the entire 32-bit physical address space is DMA-accessible. Modern SoCs (Cortex-A9 and later) typically have full 32-bit DMA capability, so no restriction is needed.

### Computing arm_dma_pfn_limit

```c
arm_dma_pfn_limit = arm_dma_limit >> PAGE_SHIFT;  /* PAGE_SHIFT = 12 */
```

Convert bytes to page frame numbers for use in zone setup:
- `arm_dma_limit = 0x0FFFFFFF` → `arm_dma_pfn_limit = 0x0FFFFFFF >> 12 = 0xFFFF`
- `arm_dma_limit = 0xFFFFFFFF` → `arm_dma_pfn_limit = 0xFFFFF` (max 32-bit PFN)

---

## 4. How arm_dma_limit Is Used Later

### In zone_sizes_init() — Called from bootmem_init() → paging_init()

```c
static void __init zone_sizes_init(unsigned long min, unsigned long max_low,
    unsigned long max_high)
{
    unsigned long max_zone_pfn[MAX_NR_ZONES] = { 0 };

#ifdef CONFIG_ZONE_DMA
    max_zone_pfn[ZONE_DMA] = min(arm_dma_pfn_limit, max_low);
#endif
    max_zone_pfn[ZONE_NORMAL] = max_low;
#ifdef CONFIG_HIGHMEM
    max_zone_pfn[ZONE_HIGHMEM] = max_high;
#endif
    free_area_init(max_zone_pfn);
}
```

`ZONE_DMA` is capped at `min(arm_dma_pfn_limit, max_low)` — it cannot extend into highmem, and it is bounded by the hardware's DMA limit.

### In dma_contiguous_reserve() — Called from arm_memblock_init()

```c
dma_contiguous_reserve(arm_dma_limit);
```

Reserves a physically contiguous region (for CMA — Contiguous Memory Allocator) within the DMA zone. The `arm_dma_limit` tells the CMA allocator where the top of the DMA zone is.

### In GFP_DMA Allocations

When drivers call `kmalloc(..., GFP_DMA)`, the buddy allocator only returns pages from `ZONE_DMA`. The zone boundary (from `zone_sizes_init`) enforces that all returned pages have physical addresses ≤ `arm_dma_pfn_limit << PAGE_SHIFT`.

---

## 5. Machine Descriptor: `dma_zone_size` Field

```c
/* arch/arm/include/asm/mach/arch.h */
struct machine_desc {
    ...
    phys_addr_t     dma_zone_size;   /* DMA zone size (0 = full 32-bit range) */
    ...
};
```

Examples:
| Board | SoC | dma_zone_size | Reason |
|-------|-----|---------------|--------|
| Raspberry Pi 1 | BCM2835 | `SZ_256M` (256MB) | GPU can only DMA into bottom 256MB |
| i.MX51 EVK | i.MX51 | `SZ_256M` | Legacy DMA engine 28-bit address limit |
| Most modern SoCs | various | `0` (default) | Full 32-bit DMA capable |

---

## 6. Call Tree (Bottom-Up)

```
arm_dma_limit / arm_dma_pfn_limit       (global variables, arch/arm/mm/init.c)
        ▲
        │ written by
setup_dma_zone(mdesc)                   arch/arm/mm/init.c:97
        ▲
        │ called from
setup_arch()                            arch/arm/kernel/setup.c:1151

        │ (later consumed by)
zone_sizes_init()                       arch/arm/mm/init.c
  └─ called from bootmem_init()
       └─ called from paging_init()

dma_contiguous_reserve(arm_dma_limit)   kernel/dma/contiguous.c
  └─ called from arm_memblock_init()
```

---

## 7. What Happens in Hardware

`setup_dma_zone()` is a **pure software operation** — it sets two global integer variables. No hardware registers are written, no page tables are modified. The hardware impact is indirect:

1. Later, `zone_sizes_init()` calls `free_area_init()` which divides physical pages into zones.
2. The buddy allocator enforces zone constraints in hardware-observable ways: `GFP_DMA` allocations physically land at low addresses where the DMA engine can reach.
3. If DMA zone setup is wrong (limit too high), DMA engines may write to physical addresses they cannot reach → **data corruption**, not a crash.

---

## 8. ZONE_DMA vs ZONE_DMA32 vs ZONE_NORMAL

| Zone | ARM32 typical use | Physical range |
|------|------------------|----------------|
| ZONE_DMA | Old DMA engines with 24-bit or 28-bit address limit | 0 – arm_dma_limit |
| ZONE_NORMAL | Main kernel allocations | arm_dma_limit+1 – arm_lowmem_limit |
| ZONE_HIGHMEM | Pages not permanently mapped to kernel VA | arm_lowmem_limit+1 – max_pfn |

ARM64 uses `ZONE_DMA` for devices that require memory below 1GB (e.g., PCIe DMA 32-bit limit) and `ZONE_DMA32` for 32-bit DMA-capable devices, with `ZONE_NORMAL` for everything else.

---

## 9. Interview Q&A

**Q1: What is the DMA zone and why does ARM32 need it?**
> ZONE_DMA is a pool of physically low memory reserved for devices that cannot address all of RAM. Early ARM SoCs (and some still today) have DMA engines with address bus widths smaller than the full physical address width. For example, a 28-bit DMA engine on a 32-bit physical bus can only address the bottom 256MB. `setup_dma_zone()` records this limit so the kernel allocator can satisfy `GFP_DMA` requests from within that constrained range.

**Q2: What happens if setup_dma_zone() is not called (or sets the wrong limit)?**
> If the limit is set too high, `zone_sizes_init()` creates a `ZONE_DMA` that extends beyond what the DMA engine can reach. A driver requesting `GFP_DMA` memory might receive a page at, say, 0x20000000, which a 28-bit DMA engine cannot address (max 0x0FFFFFFF). The DMA transfer would write to the wrong physical address, causing silent data corruption — one of the most dangerous class of hardware bugs.

**Q3: Why is arm_dma_limit set relative to PHYS_OFFSET?**
> DMA engines address physical memory starting from the first byte of DRAM, which is `PHYS_OFFSET`. If `PHYS_OFFSET = 0x80000000` and `dma_zone_size = 256MB`, then `arm_dma_limit = 0x8FFFFFFF` — the first 256MB of physical RAM starting from the DRAM base, not from physical address 0. This correctly handles systems where DRAM starts above zero.

**Q4: How is arm_dma_pfn_limit related to arm_dma_limit?**
> `arm_dma_pfn_limit = arm_dma_limit >> PAGE_SHIFT` converts the byte-address limit to a page frame number. The memory zone system (buddy allocator) works in page frames, not bytes. `zone_sizes_init()` uses `max_zone_pfn[]` in PFN units, so the conversion is necessary.

**Q5: What is CONFIG_ZONE_DMA and when would you disable it?**
> `CONFIG_ZONE_DMA=y` enables the DMA zone. If all DMA-capable devices on the target platform can address all of RAM (e.g., a modern SoC with IOMMU), you can set `CONFIG_ZONE_DMA=n`. This simplifies the zone layout (one fewer zone) and avoids the overhead of tracking DMA zone pressure. Embedded systems targeting specific hardware often disable it to reduce kernel complexity.

**Q6: What is the CMA (Contiguous Memory Allocator) and how does setup_dma_zone affect it?**
> CMA reserves a contiguous physical memory region for drivers that need large, physically contiguous DMA buffers (cameras, GPU, video codecs). `arm_memblock_init()` calls `dma_contiguous_reserve(arm_dma_limit)` which places the CMA reservation within the DMA zone. If `arm_dma_limit` is too small, the CMA region might end up outside the range the DMA engine can reach.
