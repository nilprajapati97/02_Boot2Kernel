# adjust_lowmem_bounds() First Call — ARM32 vs ARM64 Design Details

## 1. Does ARM64 Have adjust_lowmem_bounds()?

**No.** `adjust_lowmem_bounds()` is **ARM32-specific**. ARM64 does not have this function.

The concept of "lowmem vs highmem boundary" is a **32-bit virtual address space problem**. ARM64's 48-bit virtual address space is large enough to directly map all physical RAM without any conflict with vmalloc or highmem.

---

## 2. Root Cause: 32-bit VA Space Crunch

### ARM32 Virtual Address Space (32-bit, 3G/1G split)

```
0x00000000  ┌──────────────────────────────────────┐
            │ User Space                           │  3GB
            │ (processes, mmap, libraries)         │
0xC0000000  ├──────────────────────────────────────┤  PAGE_OFFSET
            │ Lowmem                               │  direct-mapped physical RAM
            │ (kernel code + data + struct pages)  │  limited by arm_lowmem_limit
arm_lowmem  ├────────────────────────────────────── ┤
            │ VMALLOC_OFFSET (8MB guard)           │
            ├──────────────────────────────────────┤  VMALLOC_START
            │ vmalloc / ioremap region             │  240MB (default)
            │ (kernel VA for dynamic mappings)     │
            ├──────────────────────────────────────┤  VMALLOC_END
            │ pkmap (HIGHMEM kmap region)          │  2MB (if CONFIG_HIGHMEM)
            ├──────────────────────────────────────┤
            │ fixmap                               │  at top of 32-bit space
0xFFFFFFFF  └──────────────────────────────────────┘
```

**The conflict**: If physical RAM is large (e.g., 2GB), naively mapping it all from 0xC0000000 would require 2GB of kernel VA — but the kernel only has 1GB of VA from 0xC0000000 to 0xFFFFFFFF, minus vmalloc, minus fixmap, minus modules.

`adjust_lowmem_bounds()` computes the maximum physical address that fits in the available kernel VA without colliding with vmalloc.

### ARM64 Virtual Address Space (48-bit, canonical halves)

```
0x0000000000000000  ┌──────────────────────────────────┐
                    │ User Space (256 TB)              │  Lower VA canonical half
0x0000FFFFFFFFFFFF  ├──────────────────────────────────┤
                    │ Non-canonical (unusable)         │
0xFFFF000000000000  ├──────────────────────────────────┤
                    │ vmalloc / ioremap (128 TB)       │  Upper VA canonical half
                    ├──────────────────────────────────┤
                    │ vmemmap (struct page array)      │
                    ├──────────────────────────────────┤
                    │ Direct linear map (1 TB+)        │  All physical RAM mapped here
                    ├──────────────────────────────────┤  PAGE_OFFSET
                    │ Kernel code + data               │  KIMAGE_VADDR
0xFFFFFFFFFFFFFFFF  └──────────────────────────────────┘
```

ARM64 has **128TB for direct map** — enough for all current physical memory. The direct map and vmalloc are in completely separate 64-bit regions. No conflict possible.

---

## 3. ARM32 Memory Calculation vs ARM64

### ARM32: adjust_lowmem_bounds() Formula

$$\text{vmalloc\_limit} = \text{VMALLOC\_END} - \text{vmalloc\_size} - \text{VMALLOC\_OFFSET} - \text{PAGE\_OFFSET} + \text{PHYS\_OFFSET}$$

This converts the vmalloc start virtual address back to a physical limit.

| Variable | ARM32 Typical Value |
|----------|-------------------|
| VMALLOC_END | 0xFF800000 |
| vmalloc_size | 240MB (default) |
| VMALLOC_OFFSET | 8MB |
| PAGE_OFFSET | 0xC0000000 |
| PHYS_OFFSET | 0x00000000 (typical) |
| **Result (vmalloc_limit)** | **0x40000000 = 1GB** |

Only 1GB of RAM can be directly mapped as lowmem with default settings.

### ARM64: No Calculation Needed

ARM64's `arm64_memblock_init()` simply maps all physical memory in the direct linear map:

```c
/* arch/arm64/mm/init.c */
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_text);
    phys_addr_t kernel_end   = __pa_symbol(__init_begin);
    struct memblock_region *reg;

    /* Map all available physical memory */
    for_each_available_child_of_node(...) {
        map_memory_region(start, end - start);
    }
}
```

All RAM is in the direct map. No lowmem/highmem split. No `adjust_lowmem_bounds()`.

---

## 4. vmalloc Window Comparison

| | ARM32 | ARM64 |
|--|-------|-------|
| vmalloc VA range | ~240MB (tunable via `vmalloc=`) | 128 TB |
| Conflict with direct map | Yes — shared 1GB kernel VA | No — separate 64-bit regions |
| vmalloc= boot param | Critical (affects lowmem size) | Not needed |
| ioremap destination | vmalloc region (constrained) | vmalloc region (vast) |
| Module load address | MODULES_VADDR = PAGE_OFFSET - 16MB | MODULES_VADDR = KIMAGE_VADDR - 128MB |

On ARM32, `vmalloc=` is frequently tuned for systems with device drivers that need large ioremap windows (e.g., GPU with large MMIO BAR). On ARM64, there is essentially unlimited vmalloc space.

---

## 5. highmem: ARM32 Only

| Concept | ARM32 | ARM64 |
|---------|-------|-------|
| highmem exists | Yes (CONFIG_HIGHMEM) | **No** |
| Why | VA space too small to map all physical RAM | 64-bit VA maps all RAM directly |
| highmem access | kmap() / kmap_atomic() | Not needed |
| ZONE_HIGHMEM | Yes | No |
| high_memory global | Set by adjust_lowmem_bounds | Not used |
| `/proc/meminfo` HighTotal | Non-zero on high-RAM ARM32 | Always 0 |

**highmem on ARM32**: If physical RAM exceeds the lowmem limit, the excess is "highmem". Kernel cannot access these pages via direct pointer — must call `kmap()` to get a temporary virtual mapping.

**ARM64**: With 128TB direct map, all physical RAM is always accessible by kernel pointer. `kmap()` is a no-op on ARM64 (`kmap() = page_to_virt(page)`).

---

## 6. PMD Alignment Requirement

Both ARM32 and ARM64 require section/PMD-aligned memory for the initial page table setup, but they differ:

| | ARM32 | ARM64 |
|--|-------|-------|
| PMD size | 2MB (LPAE) or 1MB (non-LPAE) | 2MB (4KB pages) |
| Alignment required | Yes — first block must be PMD-aligned | Yes — but enforced differently |
| Non-aligned prefix handling | `memblock_mark_nomap()` | Not an issue (virtual addresses flexible) |

`adjust_lowmem_bounds()` on ARM32 marks non-PMD-aligned starts as NOMAP because ARM32's `prepare_page_table()` clears page table entries in PMD granularity — a non-aligned start would require an allocation during table setup, which is not safe at this early stage.

---

## 7. Concrete Lowmem/Highmem Example

### ARM32 with 1GB RAM, PAGE_OFFSET=0xC0000000

```
Physical RAM:    0x00000000 – 0x3FFFFFFF (1GB)
PHYS_OFFSET:     0x00000000
vmalloc_limit:   0x40000000 (1GB)
arm_lowmem_limit: 0x3FFFFFFF

Kernel VA mapping:
  0xC0000000 – 0xFFFFFFFF (1GB kernel VA)
  0xC0000000 – 0xFFF00000 → maps physical 0x00000000 – 0x3EFFFFFF (lowmem ~1GB)
  0xFFF00000 – 0xFFF7FFFF → VMALLOC_OFFSET (8MB guard)
  0xFFF80000 – 0xFFFFFFFF → vmalloc (0.5MB, very tight!)
```

This example shows why `vmalloc=` tuning matters: if arm_lowmem_limit is too close to VMALLOC_START, the vmalloc window shrinks dangerously.

### ARM64 with 4GB RAM

```
Physical RAM:    0x00000000 – 0xFFFFFFFF (4GB)
Direct map:      0xFFFF800000000000 – 0xFFFF8000FFFFFFFF (4GB directly mapped)
vmalloc:         0xFFFF000000000000 – 0xFFFF7FFFFFFFFFFF (128TB)
No highmem zone. All 4GB directly accessible.
arm_lowmem_limit: not used.
```

---

## 8. Comparison Table

| Feature | ARM32 adjust_lowmem_bounds | ARM64 equivalent |
|---------|---------------------------|-----------------|
| Function | `adjust_lowmem_bounds()` | None needed |
| Problem solved | 32-bit VA crunch | N/A (48-bit VA) |
| vmalloc conflict | Yes | No |
| highmem zone created | Yes (if RAM > lowmem limit) | Never |
| `arm_lowmem_limit` global | Yes, critical | Not used |
| `high_memory` global | Set here | Not set from this function |
| PMD alignment | Enforced (first block) | Not constrained the same way |
| `vmalloc=` boot param | Important for tuning | Irrelevant |
| Called twice | Yes (before + after memblock init) | N/A |
| Source file | `arch/arm/mm/mmu.c` | No equivalent |
