# KASAN Early Boot Shadow Mapping

## The Bootstrap Problem

`kasan_early_init` must solve a chicken-and-egg problem:

```
PROBLEM:
    kasan_early_init itself is compiled with KASAN instrumentation
    ↓
    It contains kasan_check_read/write calls
    ↓
    Those accesses try to read shadow memory
    ↓
    Shadow memory isn't mapped yet!
    ↓
    PAGE FAULT → crash before exception handlers are ready
```

**Solution**: `kasan_early_init` is marked `__no_sanitize_address`:
```c
// arch/arm64/mm/kasan_init.c
void __init __no_sanitize_address kasan_early_init(void)
{
    // This function has NO KASAN instrumentation
    // It's safe to run even without shadow mapping
    ...
}
```

The `__no_sanitize_address` attribute tells GCC/Clang to skip KASAN
instrumentation for this specific function.

---

## Page Table Manipulation at Boot Time

At the time `kasan_early_init` runs, the kernel is using early identity-mapped
page tables (set up in `__create_page_tables`). Shadow memory must be mapped
using these same tables:

```c
void __init kasan_early_init(void)
{
    phys_addr_t shadow_start = (unsigned long)kasan_mem_to_shadow((void *)KIMAGE_VADDR);
    phys_addr_t shadow_end   = (unsigned long)kasan_mem_to_shadow((void *)_end);
    
    // Map shadow for kernel image only (we don't have full memmap yet):
    kasan_map_shadow_early(shadow_start, shadow_end - shadow_start,
                           pfn_to_phys(virt_to_pfn(kasan_early_shadow_page)));
}
```

The early shadow mapping uses `kasan_early_shadow_page` — a single physical
page containing all zeros, shared (mapped read-only) for all shadow entries:

```
Virtual address space:

0xfffffc00_00000000  ← KASAN_SHADOW_START
                       ↓ PMD/PGD entries
                     [  zero page  ]  ← all entries point to same kasan_zero_page
                     [  zero page  ]
                     ...
                     [ shadow pages ]  ← actual kernel image shadow
                     ...
0xfffffdff_ffffffff  ← KASAN_SHADOW_END
```

---

## ARM64 Page Table Walk for KASAN Shadow

```
4-level page table (48-bit VA, 4K pages):

TTBR1_EL1 → PGD (L0) → PUD (L1) → PMD (L2) → PTE (L3) → Physical page

For shadow region:
    VA[47:39] selects PGD entry → kasan_zero_pud_pmd_pte structures
    VA[38:30] selects PUD entry
    VA[29:21] selects PMD entry
    VA[20:12] selects PTE entry → points to kasan_zero_page

Architecture of zero page mapping:
                                              ┌──────────────────┐
    TTBR1_EL1                                 │  Physical Page   │
        │                                     │  (kasan_zero_page│
        ▼                                     │   = all zeros)   │
    PGD[N]──► kasan_zero_pgd                  └──────────────────┘
                │                                      ▲
                ▼                                      │
            PUD[N]──► kasan_zero_pud             ...PTE entries...
                │                                      │
                ▼                              kasan_zero_pte[*]─┘
            PMD[N]──► kasan_zero_pmd
```

After `kasan_init()` (Phase 2), real per-page shadow pages replace the
zero pages, and shadow bytes are populated with actual allocation state.

---

## `kasan_map_shadow` Implementation

```c
// arch/arm64/mm/kasan_init.c
static void __init kasan_map_shadow(phys_addr_t start, phys_addr_t size,
                                     phys_addr_t phys)
{
    unsigned long addr = (unsigned long)start;
    unsigned long end  = addr + size;
    pgd_t *pgdp;
    
    pgdp = pgd_offset_k(addr);
    
    do {
        next = pgd_addr_end(addr, end);
        kasan_pgd_populate(pgdp, addr, next, phys, NODE_DATA(0)->node_id);
    } while (pgdp++, addr = next, addr != end);
    
    // flush_tlb_all() - not needed at early boot, TLBs should be clean
}
```

The key: only map the shadow for the kernel IMAGE range during early init.
The full shadow (covering all RAM) is mapped later in `kasan_init`.

---

## Why In `__primary_switched`, Not `start_kernel`?

`kasan_early_init` must run in `__primary_switched` (assembly, before C entry) because:

1. **C code is already KASAN-instrumented**: The first C function called
   (`start_kernel`) contains KASAN checks. Shadow must be mapped BEFORE
   `start_kernel` is called.

2. **Stack is KASAN-monitored**: KASAN instruments stack variable accesses.
   The C stack frame created by `stp x29, x30` is monitored by KASAN — the
   shadow for the stack pages must be valid.

3. **Exception handlers are KASAN-instrumented**: Once VBAR_EL1 is set, any
   exception would execute KASAN-instrumented handler code.

Timeline in `__primary_switched`:
```
1. init_cpu_task      ← stack established (needs shadow later)
2. Set VBAR_EL1       ← exception handlers installed (KASAN-instrumented!)
3. stp x29, x30       ← C frame (KASAN-tracked)
4. str_l x21, __fdt   ← store (KASAN checks this!)
5. kimage_voffset     ← more C-style operations
6. set_cpu_boot_mode  ← C function call!
7. kasan_early_init   ← KASAN shadow mapped NOW
8. finalise_el2       ← C function call (KASAN instrumented)
9. bl start_kernel    ← Safe: shadow is mapped
```

Wait — but steps 6 and earlier also call C functions. These are __no_sanitize_address
functions, ensuring KASAN doesn't instrument them.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASAN (Kernel Address SANitizer) can use two modes on ARM64:
- Software KASAN (CONFIG_KASAN_GENERIC, CONFIG_KASAN_SW_TAGS): every memory access is instrumented by the compiler to check a shadow byte before the access. The shadow memory occupies 1/8 of the kernel address space (one shadow byte per 8 bytes of real memory).
- Hardware KASAN (CONFIG_KASAN_HW_TAGS, ARMv8.5 MTE): uses ARM Memory Tagging Extension. Every 16-byte-aligned allocation gets a 4-bit tag in the address (VA bits 59:56, using the TBI/Top Byte Ignore feature) and a matching tag stored in a special memory tag granule. The CPU checks tags on every load/store automatically.

### Kernel Perspective (Linux ARM64)
KASAN shadow memory is mapped during early boot in kasan_early_init() (arch/arm64/mm/kasan_init.c). For software KASAN, the shadow region at KASAN_SHADOW_START is mapped before start_kernel. For hardware KASAN (MTE), the kernel sets SCR_EL3.ATA and SCTLR_EL1.ATA to enable tag checking. KASAN errors generate a kernel panic (or WARN, depending on config) when an out-of-bounds or use-after-free access is detected. This is critical for kernel security.

### Memory Perspective (ARMv8 Memory Model)
MTE (Memory Tagging Extension, ARMv8.5) adds a 4-bit tag to each 16-byte granule of memory. The tag is stored in a separate physical memory region (tag memory) alongside the data memory. On each load/store, the CPU compares the tag in the VA (bits 59:56) with the tag in tag memory: a mismatch raises a tag check fault. This is transparent to the page tables (tags are handled by the memory controller, not the MMU). The MAIR_EL1 must set the TAGGED attribute for memory regions where MTE is active.