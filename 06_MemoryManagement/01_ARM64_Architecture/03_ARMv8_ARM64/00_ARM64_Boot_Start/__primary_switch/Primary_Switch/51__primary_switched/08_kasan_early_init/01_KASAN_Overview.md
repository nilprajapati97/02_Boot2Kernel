# KASAN Early Init — Overview

## The Conditional Call

```asm
// arch/arm64/kernel/head.S __primary_switched:
#ifdef CONFIG_KASAN
    bl      kasan_early_init
#endif
```

`kasan_early_init` only exists when `CONFIG_KASAN=y`. It's one of the last
calls in `__primary_switched` before `finalise_el2` and `start_kernel`.

---

## What KASAN Is

KASAN = **Kernel Address SANitizer**. A compile-time instrumentation framework
that detects:

1. **Use-after-free**: accessing memory after it was freed
2. **Buffer overflow**: accessing memory beyond allocated bounds
3. **Stack buffer overflow**: exceeding local variable allocation on stack
4. **Global variable overflow**: accessing beyond global array bounds
5. **Use-before-initialization**: reading uninitialized memory (with some configs)

KASAN originated in user-space (AddressSanitizer) and was ported to the Linux
kernel. It is a DEBUGGING tool — it is NOT enabled in production kernels due to
2–3× memory overhead and 10–50% performance overhead.

---

## How KASAN Works — Shadow Memory

KASAN uses a "shadow memory" region to track the validity of each byte:

```
For every 8 bytes of real memory, KASAN maintains 1 byte of shadow memory:
    Real memory byte:  valid or invalid?
    Shadow byte = 0:   all 8 bytes are valid (accessible)
    Shadow byte = N (1-7): first N bytes valid, rest are invalid (partial allocation)
    Shadow byte = 0xFF: all bytes invalid (freed, out-of-bounds, etc.)

Shadow memory layout on ARM64:
    Real memory:   0xffff800000000000 - 0xfffffdffffffffff  (linear map + kernel)
    Shadow memory: 0xfffffc0000000000 - 0xfffffdffffffffff  (1/8 of real memory size)
```

The compiler inserts instrumentation around every memory access:
```c
// Compiler inserts before: ptr[i] = value
if (*shadow_address_of(ptr + i) != 0)
    kasan_report_error(ptr + i, ...);
```

This check adds ~2 instructions per memory access — hence the 10–50% overhead.

---

## `kasan_early_init` — What It Does

```c
// arch/arm64/mm/kasan_init.c
void __init kasan_early_init(void)
{
    phys_addr_t pa_start = __pa_symbol(_text);
    phys_addr_t pa_end   = __pa_symbol(_end);
    
    /*
     * Map the shadow memory for the early kernel code.
     * At this point, the full KASAN shadow isn't set up yet —
     * we just need to prevent faults on shadow accesses for
     * the kernel image range.
     */
    BUILD_BUG_ON(!IS_ALIGNED(KASAN_SHADOW_START, PGDIR_SIZE));
    BUILD_BUG_ON(!IS_ALIGNED(KASAN_SHADOW_END + 1, PGDIR_SIZE));
    
    kasan_map_shadow(pa_start, pa_end - pa_start,
                     kasan_zero_page);  // map all shadow to "valid" initially
}
```

`kasan_early_init` does the MINIMAL setup needed for KASAN to not crash during
the rest of `__primary_switched` and early `start_kernel`:
1. Maps the KASAN shadow region for the kernel image into the page tables
2. Maps shadow to `kasan_zero_page` (all-zero = "all valid") initially
3. Prevents page faults when KASAN-instrumented code accesses shadow memory

---

## KASAN Initialization Phases

```
Phase 1: kasan_early_init (in __primary_switched)
    - Map shadow for kernel text/data (minimal)
    - All shadow = 0 (all valid — no detection yet)
    - Prevents crashes during early boot

Phase 2: kasan_init (in setup_arch → mm_init)
    - Map shadow for ALL physical RAM
    - Initialize shadow to proper values (KASAN_BYTE_ACCESSIBLE, etc.)
    - Enable actual violation detection

Phase 3: Runtime (after init)
    - KASAN intercepts all kmalloc/kfree operations
    - Shadow bytes updated on alloc (0 = valid) and free (0xFF = invalid)
    - Runtime reports on violation
```

Without `kasan_early_init`, any KASAN-instrumented code in `__primary_switched`
or early `start_kernel` would try to access shadow memory that isn't mapped,
causing a page fault before the exception handler is fully ready.

---

## KASAN Modes on ARM64

ARM64 supports three KASAN variants:

| Mode | Config | Shadow ratio | Speed overhead | Description |
|---|---|---|---|---|
| Generic | `CONFIG_KASAN_GENERIC` | 1/8 | 15–50% | Software shadow checks |
| Software tag | `CONFIG_KASAN_SW_TAGS` | 1/16 | 10–20% | Uses top byte of pointer |
| Hardware tag | `CONFIG_KASAN_HW_TAGS` | none | 1–5% | Uses ARM MTE hardware |

**Hardware Tag-Based KASAN** uses ARM Memory Tagging Extension (MTE, ARMv8.5-A):
- Tags embedded in physical memory (2 bits per 16-byte granule)
- Hardware checks tag on every load/store — no software overhead
- `kasan_early_init` for HW tags: different code path (MTE initialization)

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