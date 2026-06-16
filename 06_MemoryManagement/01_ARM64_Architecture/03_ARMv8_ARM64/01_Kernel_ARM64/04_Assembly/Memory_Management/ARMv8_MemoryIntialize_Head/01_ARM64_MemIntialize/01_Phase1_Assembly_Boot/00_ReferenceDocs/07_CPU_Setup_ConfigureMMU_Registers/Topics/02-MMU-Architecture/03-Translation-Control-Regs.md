# Translation Control Registers

`TCR_EL1` is one of the most important registers in early ARM64 memory setup.

## Big responsibilities of `TCR_EL1`

- controls size of the address ranges behind `TTBR0_EL1` and `TTBR1_EL1`
- selects translation granule size
- selects cacheability and shareability of table walks
- enables optional top-byte-ignore behavior
- configures physical address size through the IPS field
- carries other translation-related policy bits

## Fields that `__cpu_setup` explicitly builds

### `T0SZ`
Controls the size of the lower VA region, used here for the identity map side.

### `T1SZ`
Controls the size of the upper VA region, used here for the kernel address space side.

### Granule fields
Set through `TCR_TG_FLAGS`. These depend on whether the kernel is built for 4K, 16K, or 64K pages.

### Cacheability and shareability
Set through `TCR_CACHE_FLAGS` and `TCR_SHARED`.

### `IPS`
Computed from the running CPU's physical address range capability by `tcr_compute_pa_size`.

### Optional tag-related fields
Added when KASAN software tags or MTE are configured.

## `TCR2_EL1`
This is an extension register for newer features. `__cpu_setup` uses it for things like HAFT and PIE, but only if the CPU advertises support.

## Beginner summary

If `MAIR_EL1` answers "what does this memory attribute index mean?" then `TCR_EL1` answers "how should the MMU interpret this whole translation environment?"
