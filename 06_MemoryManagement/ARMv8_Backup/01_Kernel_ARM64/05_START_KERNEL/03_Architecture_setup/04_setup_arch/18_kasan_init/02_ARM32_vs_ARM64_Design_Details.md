# kasan_init() — ARM32 vs ARM64 Design Details

## 1. KASAN Support Status

| | ARM32 | ARM64 |
|--|-------|-------|
| KASAN supported | Yes (CONFIG_KASAN) | Yes (CONFIG_KASAN) |
| kasan_init() exists | Yes (arch/arm/mm/kasan_init.c) | Yes (arch/arm64/mm/kasan_init.c) |
| Shadow ratio | 1:8 | 1:8 |
| Shadow memory location | vmalloc region | Dedicated region in upper half |
| Impact on vmalloc | Significant (128MB from limited window) | Minimal (vast upper VA half) |
| KASAN_HW_TAGS (MTE) | No | Yes (ARM MTE, AArch64 only) |

---

## 2. Shadow Memory Layout Comparison

### ARM32 Shadow Memory

ARM32 has only 1GB of kernel VA. KASAN shadow needs 128MB (1/8 of 1GB). This is a large fraction of the constrained vmalloc window:

```
ARM32 Kernel VA (1GB total):
  0xC0000000 – 0xEFFFFFFF   Direct map (lowmem, 768MB)
  0xF0000000 – 0xF7FFFFFF   vmalloc window (128MB)
              └── KASAN shadow uses ~128MB of vmalloc space!
  0xF8000000 – 0xFEFFFFFF   More vmalloc
  0xFF000000 – 0xFFEFFFFF   modules
  0xFFF00000 – 0xFFFFFFFF   fixmap, vectors
```

KASAN on ARM32 is particularly expensive in VA space terms. This is one reason KASAN is impractical on constrained ARM32 systems.

### ARM64 Shadow Memory

ARM64 has 128TB of upper-half kernel VA. KASAN's shadow region is carved out of this vast space:

```
ARM64 Kernel VA (upper half — 128TB):
  0xFFFF000000000000 – 0xFFFF7FFFFFFFFFFF   vmalloc (128TB)
  0xFFFF800000000000 – 0xFFFFA00000000000   linear map (128TB)
  0xFFFFA00000000000 – 0xFFFFBFFFFFFFFFFF   vmemmap (struct pages)
  0xFFFFE00000000000 – 0xFFFFFFFFFFFFFFFF   kernel code + data

  KASAN shadow: dedicated range (e.g., 0xFFFFC00000000000 – 0xFFFFDFFFFFFFFFFF)
  → 32TB shadow covers all 256TB of kernel address space (1:8 ratio)
```

ARM64 KASAN shadow doesn't compete with vmalloc for VA space — separate regions.

---

## 3. KASAN Modes: Generic vs SW_TAGS vs HW_TAGS

Linux supports three KASAN modes:

### Generic KASAN (ARM32 and ARM64)

```
Mechanism: Compiler instrumentation (-fsanitize=kernel-address)
Shadow ratio: 1:8
Overhead: ~2-3x slowdown, 1/8 extra memory
Detects: use-after-free, heap/stack/global buffer overflows
ARM32: Only mode supported
ARM64: Supported but not the preferred production mode
```

### Software Tag-Based KASAN (ARM64 only)

```
Mechanism: Uses ARM64's Top Byte Ignore (TBI) feature
  TBI: CPU ignores bits[63:56] of virtual addresses for TLB lookup
  Tag: 8-bit random tag stored in bits[63:56] of pointer
  Shadow: stores expected tag; check: pointer tag == shadow tag

Shadow ratio: 1:16 (half the memory of generic KASAN)
Overhead: ~2x slowdown
ARM32: NOT supported (no TBI on ARM32)
ARM64: CONFIG_KASAN_SW_TAGS
```

### Hardware Tag-Based KASAN (ARM64 with MTE only)

```
Mechanism: Uses ARM MTE (Memory Tagging Extension) — ARMv8.5+
  Hardware: Each 16-byte memory granule has a 4-bit tag in EL1 memory
  Pointer: 4-bit tag in pointer bits[59:56] (TBI + top nibble)
  Check: hardware automatically faults on tag mismatch (no software check)

Shadow: Not needed — hardware maintains tags
Overhead: Near-zero (hardware check is part of memory access)
ARM32: NOT supported (no MTE on ARM32)
ARM64: CONFIG_KASAN_HW_TAGS (requires Cortex-A510, A710, X2, etc.)
```

---

## 4. ARM64 MTE (Memory Tagging Extension) — Hardware KASAN

MTE is an ARM64 hardware feature available from ARMv8.5:

```
Physical memory:
  Regular data: stored normally
  Tag bits: extra 4 bits per 16-byte "granule" stored in EL1 physical memory
            (transparent to application; accessed via separate instructions)

Pointer:
  bits[63:60] or [59:56]: 4-bit tag (TAGGED_ADDR_ENABLE must be set)
  bits[47:0]: actual virtual address

Hardware check:
  On every load/store, hardware compares pointer tag with memory tag
  Mismatch → memory tag fault (synchronous or asynchronous)
  No software shadow, no instrumented stores, no performance overhead
```

ARM64 `kasan_init()` with `CONFIG_KASAN_HW_TAGS`:

```c
/* arch/arm64/mm/kasan_init.c */
void __init kasan_init(void)
{
    /* Enable MTE for EL1 */
    write_sysreg_s(read_sysreg_s(SYS_SCTLR_EL1) | SCTLR_ELx_ATA,
                   SYS_SCTLR_EL1);
    isb();

    /* Configure tag fault handling */
    /* Initialize tag allocation for kmalloc/page allocator */
    kasan_hw_tags_init();
}
```

MTE makes KASAN production-ready on ARM64 — near-zero overhead means it can be enabled in shipping devices for security hardening.

---

## 5. Generic kasan_init(): ARM32 vs ARM64

### ARM32 kasan_init() (arch/arm/mm/kasan_init.c)

```c
void __init kasan_init(void)
{
    /* Shadow for modules area */
    kasan_map_populate(KASAN_SHADOW_START, ..., num_online_nodes());

    /* Zero shadow for unmapped regions */
    kasan_populate_zero_shadow(...);

    /* Real shadow pages for each memblock region */
    for_each_mem_range(i, &start, &end) {
        kasan_map_populate(shadow_start, shadow_end, NUMA_NO_NODE);
    }

    /* Poison early shadow page with KASAN_FREE_PAGE */
    memset(kasan_early_shadow_page, KASAN_FREE_PAGE, PAGE_SIZE);

    init_task.kasan_depth = 0;
}
```

### ARM64 kasan_init() (arch/arm64/mm/kasan_init.c)

```c
void __init kasan_init(void)
{
    u64 kimg_shadow_start, kimg_shadow_end;
    u64 mod_shadow_start, mod_shadow_end;
    phys_addr_t pa_start, pa_end;
    u64 i;

    /* Map shadow for kernel image */
    kimg_shadow_start = (u64)kasan_mem_to_shadow(_text) & PAGE_MASK;
    kimg_shadow_end   = round_up((u64)kasan_mem_to_shadow(_end), PAGE_SIZE);
    kasan_map_populate(kimg_shadow_start, kimg_shadow_end, NUMA_NO_NODE);

    /* Map shadow for modules */
    mod_shadow_start = (u64)kasan_mem_to_shadow((void *)MODULES_VADDR);
    mod_shadow_end   = (u64)kasan_mem_to_shadow((void *)MODULES_END);
    kasan_map_populate(mod_shadow_start, mod_shadow_end, NUMA_NO_NODE);

    /* Map shadow for each physical memory region */
    for_each_mem_range(i, &pa_start, &pa_end) {
        void *start = (void *)__phys_to_virt(pa_start);
        void *end   = (void *)__phys_to_virt(pa_end);
        kasan_map_populate((u64)kasan_mem_to_shadow(start),
                           (u64)kasan_mem_to_shadow(end), ...);
    }

    /* Zero shadow for remaining unmaapped areas */
    kasan_populate_zero_shadow(...);

    /* Disable checking and enable for init_task */
    init_task.kasan_depth = 0;
}
```

ARM64 version additionally handles KASLR: `_text` and `_end` are at randomized VAs, but `kasan_mem_to_shadow()` handles this transparently.

---

## 6. Comparison Table

| Feature | ARM32 KASAN | ARM64 KASAN |
|---------|-------------|-------------|
| Generic KASAN | Yes | Yes |
| SW Tag-Based | No | Yes (TBI) |
| HW Tag-Based (MTE) | No | Yes (ARMv8.5+) |
| Shadow VA crunch | Yes (128MB from 1GB) | No (separate region) |
| KASLR interaction | No KASLR | Handled transparently |
| Production use | Rarely (overhead, VA cost) | MTE: production-ready |
| Compiler flag | -fsanitize=kernel-address | Same |
| Shadow offset | Fixed (PAGE_OFFSET-based) | KASAN_SHADOW_OFFSET |
| Shadow pages | From vmalloc region | Dedicated KASAN region |
| ARM MTE hardware | Not available | Available from ARMv8.5 |
