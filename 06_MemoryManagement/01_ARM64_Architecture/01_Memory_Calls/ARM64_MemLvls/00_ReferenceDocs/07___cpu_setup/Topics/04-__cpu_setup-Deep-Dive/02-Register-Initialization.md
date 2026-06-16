# Register Initialization

This is the heart of `__cpu_setup`.

## Named working registers

The assembly assigns names to three temporary general-purpose registers:

- `x17` as `mair`
- `x16` as `tcr`
- `x15` as `tcr2`

These hold the values that will later be written to EL1 system registers.

## `MAIR_EL1`

The code loads `MAIR_EL1_SET` into `mair`.

This defines the memory attribute encodings that Linux wants active when page-table entries reference attribute indices.

## `TCR_EL1`

The code then builds a large combined value for `tcr` from:

- `TCR_T0SZ(IDMAP_VA_BITS)`
- `TCR_T1SZ(VA_BITS_MIN)`
- `TCR_CACHE_FLAGS`
- `TCR_SHARED`
- `TCR_TG_FLAGS`
- `TCR_KASLR_FLAGS`
- `TCR_EL1_AS`
- `TCR_EL1_TBI0`
- `TCR_EL1_A1`
- `TCR_KASAN_SW_FLAGS`
- `TCR_MTE_FLAGS`

## Why these pieces are combined here

This one expression collects the kernel's default translation policy:

- idmap size
- kernel high VA size
- page-size model
- table-walk memory behavior
- ASID behavior
- top-byte-ignore and tagging behavior
- optional hardening and sanitizer support

## `TCR2_EL1`

The code starts `tcr2` at zero because not every CPU supports or needs the extended translation control register. Later feature checks add bits only when appropriate.
