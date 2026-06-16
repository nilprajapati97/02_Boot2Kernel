# arm_memblock_init() — ARM32 vs ARM64 Design Details

## 1. Does ARM64 Have arm_memblock_init()?

**No.** ARM64 has its own function: `arm64_memblock_init()` in `arch/arm64/mm/init.c`.

The functions share the same purpose (reserve memory before building page tables) but differ significantly in implementation due to:
- ARM64 KASLR (randomized kernel image placement)
- ARM64 linear map randomization
- ACPI support (ARM64 servers)
- No highmem/lowmem split on ARM64

---

## 2. ARM32 arm_memblock_init() vs ARM64 arm64_memblock_init()

### ARM32 arm_memblock_init() (arch/arm/mm/init.c)

```c
void __init arm_memblock_init(const struct machine_desc *mdesc)
{
    /* Reserve kernel text+data+bss */
    memblock_reserve(__pa(KERNEL_START), KERNEL_END - KERNEL_START);

    reserve_initrd_mem();
    arm_mm_memblock_reserve();     /* ARM32 page tables */

    if (mdesc->reserve)
        mdesc->reserve();          /* board-specific */

    early_init_fdt_scan_reserved_mem();
    dma_contiguous_reserve(arm_dma_limit);
    arm_memblock_steal_permitted = false;
}
```

### ARM64 arm64_memblock_init() (arch/arm64/mm/init.c)

```c
void __init arm64_memblock_init(void)
{
    /* Reserve [0..TEXT_OFFSET] hole (not always zero) */
    if (!IS_ENABLED(CONFIG_RANDOMIZE_BASE))
        memblock_reserve(0, __pa(KERNEL_START));

    /* Reserve kernel image */
    memblock_reserve(__pa(KERNEL_START), (u64)(_end - _text));

    /* Handle KASLR — reserve module region if outside linear map */
    if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
        extern u16 memstart_offset_seed;
        u64 range = linear_region_size -
                    BIT(cpuid_feature_extract_unsigned_field(...));
        memstart_addr -= ((u64)memstart_offset_seed * range) >> 16;
    }

    /* Reserve for swapper page directory */
    memblock_reserve(__pa(swapper_pg_dir), PAGE_SIZE);

    /* Process FDT /reserved-memory nodes */
    early_init_fdt_scan_reserved_mem();

    /* Reserve initrd */
    reserve_initrd_mem();

    /* ACPI tables reservation (ARM64 servers) */
    if (!acpi_disabled)
        acpi_fadt_sanity_check();

    /* Reserve crashkernel */
    reserve_crashkernel();

    /* EFI boot data */
    efi_reserve_boot_services();

    /* CMA */
    dma_contiguous_reserve(arm64_dma_phys_limit);

    /* memstart_addr randomization */
    memblock_dump_all();
}
```

### Key Differences

| Feature | ARM32 arm_memblock_init | ARM64 arm64_memblock_init |
|---------|------------------------|--------------------------|
| KASLR | No | Yes (randomizes memstart_addr) |
| ACPI support | Minimal | Full (acpi_fadt_sanity_check) |
| Machine descriptor | Yes (mdesc->reserve) | No machine descriptor |
| Page table reservation | arm_mm_memblock_reserve | swapper_pg_dir only |
| EFI boot services | In arm_efi_init | efi_reserve_boot_services here |
| arm_lowmem_limit | Used by ARM32 | Not used |
| highmem zone | Possible | Never |
| arm_memblock_steal | Has guard flag | No equivalent guard |

---

## 3. memblock Architecture: Shared Foundation

Both ARM32 and ARM64 use the same `mm/memblock.c` infrastructure:

```
memblock
  ├── .memory[]   — all physical memory regions (from FDT/ACPI/UEFI)
  └── .reserved[] — reserved subsets of memory

Operations:
  memblock_add()         — add a memory region
  memblock_reserve()     — mark a region as reserved
  memblock_remove()      — remove from memory map entirely
  memblock_mark_nomap()  — exists but not mapped
  memblock_alloc()       — allocate from free (non-reserved) memory
  memblock_free()        — release a reserved region
```

The `.memory` and `.reserved` arrays have a limited initial size (128 entries each). Entries are merged when possible. Overflow causes `BUG_ON()`.

---

## 4. Kernel Image Reservation: Linker Symbols

Both ARM32 and ARM64 use linker script symbols to locate the kernel image:

### ARM32 (arch/arm/kernel/vmlinux.lds.S)
```c
#define KERNEL_START  _text          /* start of kernel code */
#define KERNEL_END    _end           /* end of BSS */

memblock_reserve(__pa(KERNEL_START), KERNEL_END - KERNEL_START);
```

### ARM64 (arch/arm64/kernel/vmlinux.lds.S)
```c
/* _text, _end from linker script */
memblock_reserve(__pa(KERNEL_START), (u64)(_end - _text));
```

The `__pa()` macro converts the virtual address (which the kernel was linked at) to the physical address where it was actually loaded by the bootloader. These are different when KASLR is active.

---

## 5. initrd Reservation

Both ARM32 and ARM64 call `reserve_initrd_mem()`:

```c
/* mm/init.c — architecture-independent */
static void __init reserve_initrd_mem(void)
{
    phys_addr_t start = phys_initrd_start;
    unsigned long size = phys_initrd_size;

    if (!size)
        return;

    if (!memblock_is_region_memory(start, size)) {
        pr_err("INITRD: 0x%08llx+0x%08lx overlaps non-memory region\n",
               ...);
        goto disable;
    }

    memblock_reserve(start, size);
    ...
}
```

The initrd physical address comes from:
- **ARM32**: `phys_initrd_start` set by ATAG or FDT `chosen.linux,initrd-start`
- **ARM64**: Same — FDT `chosen.linux,initrd-start`

---

## 6. DMA Contiguous Reserve: ARM32 vs ARM64

```c
/* ARM32: */
dma_contiguous_reserve(arm_dma_limit);
/* arm_dma_limit comes from setup_dma_zone(mdesc) */

/* ARM64: */
dma_contiguous_reserve(arm64_dma_phys_limit);
/* arm64_dma_phys_limit = min(mem_limit, DMA_BIT_MASK(32)) for most platforms */
```

ARM64 servers typically have IOMMU, which eliminates most DMA addressing constraints. CMA on ARM64 may be placed above 4GB if the device has a 64-bit DMA mask. ARM32 CMA is always below `arm_dma_limit`.

---

## 7. Machine Descriptor vs Device Tree for Reservations

ARM32 supports **both** `mdesc->reserve()` and FDT `/reserved-memory`:

```
Historical evolution:
  ┌─────────────────────────────────────────────────────────────┐
  │ Old ARM32: All reservations in mdesc->reserve() (C code)   │
  │   board.c:                                                  │
  │     .reserve = board_reserve_memory,                        │
  │   board_reserve_memory():                                   │
  │     memblock_reserve(0x1E000000, 0x2000000);               │
  └─────────────────────────────────────────────────────────────┘
                           ↓ Migration
  ┌─────────────────────────────────────────────────────────────┐
  │ Modern ARM32: Reservations in DTB /reserved-memory         │
  │   board.dts:                                               │
  │     reserved-memory {                                       │
  │         gpu@1e000000 { reg = <0x1e000000 0x2000000>; };   │
  │     };                                                      │
  │   C code: mdesc->reserve = NULL; (just use FDT)           │
  └─────────────────────────────────────────────────────────────┘
```

ARM64 never had machine descriptors — it went directly to FDT/ACPI-based reservations.

---

## 8. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Function name | `arm_memblock_init(mdesc)` | `arm64_memblock_init()` |
| Machine descriptor | Yes | No |
| KASLR support | No | Yes |
| ACPI reservations | No | Yes |
| DMA limit source | `arm_dma_limit` (from mdesc) | `arm64_dma_phys_limit` |
| Steal guard | `arm_memblock_steal_permitted` | None |
| Page table reservation | `arm_mm_memblock_reserve()` | Only `swapper_pg_dir` |
| LPAE physical addresses | Yes (if CONFIG_LPAE) | N/A (always 64-bit PA) |
| Highmem possibility | Yes | No |
