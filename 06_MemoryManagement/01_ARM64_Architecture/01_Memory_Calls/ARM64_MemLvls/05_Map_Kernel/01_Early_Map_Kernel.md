# `early_map_kernel()` — Entry Point

**Source:** `arch/arm64/kernel/pi/map_kernel.c` lines 230–290

## Purpose

`early_map_kernel` is the C entry point called from `__primary_switch` (head.S). It runs with the MMU on via the identity map, so all addresses used are physical. It sets up the environment, determines KASLR displacement, and calls `map_kernel()`.

## Function Signature

```c
asmlinkage void __init early_map_kernel(u64 boot_status, phys_addr_t fdt)
```

- `boot_status` (from `x20`): CPU boot mode (EL1 or EL2 + flags)
- `fdt` (from `x21`): Physical address of the Flattened Device Tree

## Step-by-Step

### 1. Map the FDT

```c
void *fdt_mapped = map_fdt(fdt);
```

The FDT is in memory but may not be in the identity map. `map_fdt()` adds it to `init_idmap_pg_dir` so it can be read.

### 2. Clear BSS and Page Tables

```c
memset(__bss_start, 0, (char *)init_pg_end - (char *)__bss_start);
```

This zeros everything from BSS through the end of `init_pg_dir`. Since we're running identity-mapped, these are physical addresses.

### 3. Parse CPU Feature Overrides

```c
chosen = fdt_path_offset(fdt_mapped, "/chosen");
init_feature_override(boot_status, fdt_mapped, chosen);
```

Reads the FDT `/chosen` node to check for CPU feature overrides (e.g., forcing features on/off via bootloader).

### 4. Determine VA Bits and Root Level

```c
int root_level = 4 - CONFIG_PGTABLE_LEVELS;
int va_bits = VA_BITS;

if (IS_ENABLED(CONFIG_ARM64_64K_PAGES) && !cpu_has_lva())
    va_bits = VA_BITS_MIN;
else if (IS_ENABLED(CONFIG_ARM64_LPA2) && !cpu_has_lpa2())
    va_bits = VA_BITS_MIN, root_level++;
```

If the kernel is compiled for 52-bit VA but the CPU doesn't support it, fall back to the minimum (typically 48 bits). The root level is adjusted accordingly (more levels = deeper tree = more bits).

### 5. Update TCR for Larger VA

```c
if (va_bits > VA_BITS_MIN)
    sysreg_clear_set(tcr_el1, TCR_EL1_T1SZ_MASK, TCR_T1SZ(va_bits));
```

If using more VA bits than the minimum, update TCR_EL1.T1SZ to reflect the larger kernel VA space.

### 6. KASLR Offset Computation

```c
u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);
kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
```

- Low bits come from physical placement: `pa_base % MIN_KIMG_ALIGN`
- High bits come from the random seed in the FDT
- Result: kernel shifts by a random offset while maintaining block alignment

### 7. LPA2 ID Map Remap

```c
if (IS_ENABLED(CONFIG_ARM64_LPA2) && va_bits > VA_BITS_MIN)
    remap_idmap_for_lpa2();
```

If using 52-bit physical addresses, the identity map descriptors need to be reformatted because certain PTE bits change meaning when the DS flag is set in TCR.

### 8. Call `map_kernel`

```c
va_base = KIMAGE_VADDR + kaslr_offset;
map_kernel(kaslr_offset, va_base - pa_base, root_level);
```

`va_offset = va_base - pa_base` is the virtual-to-physical offset that every address must be shifted by when creating page table entries.

## Key Variables

| Variable | Value | Purpose |
|----------|-------|---------|
| `pa_base` | `&_text` | Physical address of kernel start |
| `va_base` | `KIMAGE_VADDR + kaslr_offset` | Virtual address of kernel start |
| `va_offset` | `va_base - pa_base` | VA↔PA displacement |
| `kaslr_offset` | random | Randomization displacement |
| `root_level` | 0 or 1 | Starting level of page table walk |

## Key Takeaway

`early_map_kernel` is the decision-maker: it figures out the VA space size, KASLR offset, and whether LPA2 is needed. It then delegates the actual page table construction to `map_kernel()`. Everything runs identity-mapped, so all pointers are physical addresses.
