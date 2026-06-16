# setup_dma_zone() — ARM32 vs ARM64 Design Details

## 1. Function Presence

| | ARM32 | ARM64 |
|--|-------|-------|
| `setup_dma_zone()` function | Yes — `arch/arm/mm/init.c` | No equivalent function |
| DMA zone concept | `ZONE_DMA` (optional, `CONFIG_ZONE_DMA`) | `ZONE_DMA` + `ZONE_DMA32` |
| Machine descriptor field | `mdesc->dma_zone_size` | Not used (ACPI/FDT IOMMU tables) |
| DMA limit variable | `arm_dma_limit` (phys_addr_t) | `dma_direct_max_pfn` (generic) |

ARM64 does not have a `setup_dma_zone()` call in `setup_arch()`. DMA zone setup is handled by the generic kernel infrastructure after memory management is initialized.

---

## 2. ARM32 DMA Zone Architecture

### Physical Memory Zone Layout (ARM32)

```
Physical Memory (example: 512MB total, 256MB DMA limit):

0x00000000  ┌────────────────────────────────────┐
            │ ZONE_DMA                           │  0 – 0x0FFFFFFF (256MB)
            │ (arm_dma_pfn_limit = 0xFFFF)       │  GFP_DMA allocations from here
0x10000000  ├────────────────────────────────────┤
            │ ZONE_NORMAL                        │  0x10000000 – arm_lowmem_limit
            │ (general kernel allocations)       │
arm_lowmem  ├────────────────────────────────────┤
            │ ZONE_HIGHMEM                       │  arm_lowmem_limit – top of RAM
            │ (not permanently mapped)           │
0x20000000  └────────────────────────────────────┘
```

### ARM32 Variables Set by setup_dma_zone()

```c
/* Set by setup_dma_zone(): */
phys_addr_t  arm_dma_limit;       /* max DMA-addressable physical byte */
unsigned long arm_dma_pfn_limit;  /* max DMA page frame number */

/* Also set indirectly via arm_dma_zone_size: */
phys_addr_t  arm_dma_zone_size;   /* size of DMA zone in bytes */
```

---

## 3. ARM64 DMA Zone Architecture

### Why ARM64 Doesn't Need setup_dma_zone()

ARM64 systems use:
1. **IOMMU / SMMU (System Memory Management Unit)**: Remaps DMA addresses from device perspective. A device with 32-bit DMA capability can still reach all of RAM through the SMMU's address translation.
2. **ACPI IORT (I/O Remapping Table)** or **FDT `iommu-map`**: Describes which devices are behind which SMMU.
3. **`dma_direct_max_pfn`**: For devices not behind an SMMU, the generic DMA layer limits allocations.

On ARM64, the typical zone layout is:

```
Physical Memory (ARM64, example 4GB):

0x0000000000000000  ┌─────────────────────────────┐
                    │ ZONE_DMA                    │  0 – 1GB (for old 32-bit DMA devices)
0x0000000040000000  ├─────────────────────────────┤
                    │ ZONE_DMA32                  │  1GB – 4GB (for 32-bit bus DMA)
0x0000000100000000  ├─────────────────────────────┤
                    │ ZONE_NORMAL                 │  4GB – top of RAM
                    │ (most allocations here)     │
                    └─────────────────────────────┘
```

`ZONE_DMA` on ARM64 covers memory below 1GB (for ISA DMA legacy devices, or devices with 30-bit address limit). `ZONE_DMA32` covers memory in the 32-bit physical range for PCIe devices with 32-bit DMA.

### ARM64 Zone Setup Code Path

```c
/* arch/arm64/mm/init.c */
static void __init zone_sizes_init(void)
{
    unsigned long max_zone_pfns[MAX_NR_ZONES]  = {0};

    if (IS_ENABLED(CONFIG_ZONE_DMA)) {
        max_zone_pfns[ZONE_DMA] = PFN_DOWN(arm64_dma_phys_limit);
    }
    if (IS_ENABLED(CONFIG_ZONE_DMA32)) {
        max_zone_pfns[ZONE_DMA32] = PFN_DOWN(arm64_dma32_phys_limit);
    }
    max_zone_pfns[ZONE_NORMAL] = max_pfn;

    free_area_init(max_zone_pfns);
}
```

`arm64_dma_phys_limit` is computed in `arm64_memblock_init()` by checking the FDT and IOMMU capabilities — no machine descriptor field is needed.

---

## 4. DMA Limit Determination: ARM32 vs ARM64

| Approach | ARM32 | ARM64 |
|---------|-------|-------|
| Source of limit | `mdesc->dma_zone_size` (board-specific) | FDT IOMMU tables + ACPI IORT |
| Limit storage | `arm_dma_limit` (global) | `arm64_dma_phys_limit` (global) |
| Default (no spec) | `0xFFFFFFFF` (entire 32-bit range) | Computed from IOMMU capability |
| Machine-specific override | `mdesc->dma_zone_size` | Requires IOMMU driver changes |
| When set | Very early (`setup_dma_zone()`) | Later (`arm64_memblock_init()`) |

---

## 5. Machine Descriptor vs FDT IOMMU

### ARM32: Board Files set dma_zone_size

```c
/* arch/arm/mach-bcm2835/bcm2835.c (Raspberry Pi 1) */
DT_MACHINE_START(BCM2835, "BCM2835")
    .init_machine   = bcm2835_init,
    .dma_zone_size  = SZ_256M,    /* ← explicit DMA limit */
MACHINE_END
```

This is a **hardcoded board policy** in C code. If the hardware changes, you must update the board file and recompile.

### ARM64: IOMMU/SMMU-Driven

```dts
/* Device Tree */
smmu: iommu@e8a00000 {
    compatible = "arm,smmu-v2";
    ...
    #iommu-cells = <1>;
};

sata: sata@e0800000 {
    compatible = "generic-ahci";
    iommus = <&smmu 0x104>;  /* SATA is behind SMMU */
    ...
};
```

The SMMU driver translates 64-bit kernel addresses to whatever the device can handle. No static `dma_zone_size` needed — the SMMU provides address translation at runtime.

---

## 6. GFP_DMA Flag Behavior Comparison

| | ARM32 | ARM64 |
|--|-------|-------|
| `GFP_DMA` effect | Returns page from ZONE_DMA (≤ arm_dma_limit) | Returns page from ZONE_DMA (≤ 1GB) |
| `GFP_DMA32` | Not commonly used (single 32-bit address space) | Returns page from ZONE_DMA32 (≤ 4GB) |
| `GFP_KERNEL` | Returns from ZONE_NORMAL (or ZONE_DMA if depleted) | Returns from ZONE_NORMAL |
| IOMMU bypass | Not typically available | Available — SMMU maps any VA to device |

---

## 7. Impact of dma_zone_size on Memory Layout

### ARM32 Example: BCM2835 (Raspberry Pi 1)

```
PHYS_OFFSET = 0x00000000
dma_zone_size = 256MB = 0x10000000
arm_dma_limit = 0x0FFFFFFF

Zone layout:
  ZONE_DMA:    PFN 0x00000 – 0x0FFFF  (256MB)
  ZONE_NORMAL: PFN 0x10000 – max_low  (remaining lowmem)
  ZONE_HIGHMEM: PFN max_low – max_pfn (highmem, if any)
```

The VideoCore GPU (DMA engine) on BCM2835 can only access the bottom 256MB of physical RAM. ARM Linux enforces this via ZONE_DMA.

### ARM64 Example: Raspberry Pi 4 (BCM2711)

On RPi 4, the IOMMU (VPU firmware) provides full address mapping. ZONE_DMA32 covers 0-4GB for backward compatibility with PCIe 32-bit DMA devices. No board-specific `dma_zone_size` is set — the FDT+driver infrastructure handles it.

---

## 8. Comparison Table: setup_dma_zone() ARM32 vs ARM64

| Feature | ARM32 (setup_dma_zone) | ARM64 (no setup_dma_zone) |
|---------|------------------------|---------------------------|
| Function | `setup_dma_zone(mdesc)` | None |
| Zone names | ZONE_DMA | ZONE_DMA, ZONE_DMA32 |
| Limit source | `mdesc->dma_zone_size` | IOMMU/SMMU + FDT |
| Fallback | `0xFFFFFFFF` | `arm64_dma_phys_limit` from memory topology |
| When called | Early in setup_arch() | During zone_sizes_init() from paging_init() |
| Hardware model | DMA engine has fixed address limit | SMMU provides address translation |
| Flexibility | Low (recompile to change) | High (DT/IOMMU config) |
| Zones possible | 2-3 (DMA, NORMAL, HIGHMEM) | 3 (DMA, DMA32, NORMAL) |
| HIGHMEM zone | Yes (32-bit VA constraint) | No (64-bit VA — no need) |
