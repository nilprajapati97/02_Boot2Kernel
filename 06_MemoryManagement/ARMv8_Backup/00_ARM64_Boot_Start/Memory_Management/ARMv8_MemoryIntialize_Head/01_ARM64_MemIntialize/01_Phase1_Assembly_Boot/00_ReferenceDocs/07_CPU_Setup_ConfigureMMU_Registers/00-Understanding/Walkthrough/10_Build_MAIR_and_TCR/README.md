# 10 Build `MAIR_EL1` And `TCR_EL1`

This is the dense core of the routine.

## Register Aliases

Linux uses temporary register aliases:

- `mair = x17`
- `tcr = x16`
- `tcr2 = x15`

That choice is not architecturally important by itself. What matters is that Linux constructs the target control values in general-purpose registers first, then commits them only after runtime adjustments are complete.

## `MAIR_EL1`

Linux builds `MAIR_EL1_SET`, which defines the memory-type map used by page descriptors. The important high-level classes are:

- normal write-back cacheable memory
- normal non-cacheable memory
- device memory with stricter ordering semantics
- a normal-tagged slot that can be refined later for MTE-capable systems

Without `MAIR_EL1`, an `AttrIndx` field in a page descriptor has no concrete memory behavior.

## `TCR_EL1`

Linux builds a large initial `TCR_EL1` value containing:

- `T0SZ(IDMAP_VA_BITS)` for the idmap side
- `T1SZ(VA_BITS_MIN)` for the kernel side
- page-walk cacheability flags
- page-walk shareability flags
- translation granule flags
- Linux policy flags for KASLR, ASID selection, TBI, and tagging-related modes

This line is the policy statement for how the CPU should interpret virtual addresses once translation is active.

## Why `T1SZ` Starts Conservative

Linux uses `VA_BITS_MIN` first because the same kernel image may need to boot on hardware that does not support the largest configured virtual address size. The function starts from the safe common denominator and only expands later if runtime feature discovery allows it.