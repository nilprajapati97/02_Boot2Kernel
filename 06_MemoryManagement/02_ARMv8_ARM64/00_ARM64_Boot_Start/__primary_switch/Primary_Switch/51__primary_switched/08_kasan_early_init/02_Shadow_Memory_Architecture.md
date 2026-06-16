# KASAN Shadow Memory — Architecture Deep Dive

## Shadow Memory Address Calculation

For any kernel virtual address `addr`, the shadow address is:
```c
#define KASAN_SHADOW_SCALE_SHIFT 3  // 1 shadow byte per 8 real bytes

static inline void *kasan_mem_to_shadow(const void *addr)
{
    return (void *)((unsigned long)addr >> KASAN_SHADOW_SCALE_SHIFT)
                    + KASAN_SHADOW_OFFSET;
}
```

On ARM64 (48-bit VA, 4K pages):
```
KASAN_SHADOW_OFFSET = 0xdfffffd000000000UL  (varies by config)

Example:
    Real address:   0xffff800010000000  (linear map start)
    Shift right 3:  0x1fff100002000000
    + OFFSET:       0xdfffffd000000000
    Shadow addr:    0xfffffc0012000000
```

ASCII Memory Map with KASAN Shadow:
```
Virtual Address Space (ARM64, 48-bit VA):

0xffff_ffff_ffff_ffff ┐
                       │ EL2 + vmalloc area
0xffff_fe00_0000_0000  │
0xffff_fc00_0000_0000 ─┤ KASAN_SHADOW_START (← 1/8 of kernel VA space)
                       │
                       │  KASAN Shadow Region
                       │  (every real kernel byte has 1 shadow bit here)
                       │  Shadow byte = 0:   memory valid
                       │  Shadow byte = 0xFF: memory invalid
                       │  Shadow byte = 0x01: partial (1 byte valid)
                       │
0xffff_e000_0000_0000 ─┤ KASAN_SHADOW_END
0xffff_c000_0000_0000 ─┤ Linear map start (RAM mapped here)
                       │  Physical RAM → Virtual addresses 1:1
0xffff_8000_1000_0000 ─┤ Kernel .text (virtual address of kernel image)
                       │
0x0000_ffff_ffff_ffff  │ TTBR0 (user space, EL0)
0x0000_0000_0000_0000 ─┘
```

---

## Shadow Byte Encoding

```c
// include/linux/kasan-checks.h (simplified):
#define KASAN_BYTE_ACCESSIBLE         0x00  // all 8 bytes accessible
#define KASAN_PARTIAL_1               0x01  // only first 1 byte accessible
#define KASAN_PARTIAL_2               0x02  // only first 2 bytes accessible
...
#define KASAN_PARTIAL_7               0x07  // only first 7 bytes accessible
#define KASAN_STACK_LEFT              0xF1  // left redzone (stack)
#define KASAN_STACK_MID               0xF2  // middle redzone
#define KASAN_STACK_RIGHT             0xF3  // right redzone
#define KASAN_STACK_PARTIAL           0xF4  // partial redzone
#define KASAN_USE_AFTER_SCOPE         0xF8  // out-of-scope local variable
#define KASAN_FREE_PAGE               0xFF  // freed slab page
#define KASAN_ALLOCA_LEFT             0xCA  // left alloca redzone
#define KASAN_ALLOCA_RIGHT            0xCB  // right alloca redzone
```

Example 12-byte allocation in an 8-byte-aligned slot:
```
Real memory (16 bytes):
+--------+--------+
| OBJECT |REDZONE |
| 12bytes| 4bytes |
+--------+--------+

Shadow (2 bytes):
+------+------+
| 0x04 | 0xFF |  ← 0x04 = first 4 bytes valid (but slot is 8B, so 4 bytes redzone)
+------+------+
  Actually for 12 bytes:
  First 8 bytes: shadow = 0x00 (all valid)
  Next 4 bytes:  shadow = 0x04 (4 bytes valid)
  Redzone after: shadow = 0xFC (heap right redzone)
```

---

## `kasan_zero_page` — The Early Boot Shadow

During `kasan_early_init`, all shadow is mapped to `kasan_zero_page`:
```c
// arch/arm64/mm/kasan_init.c
static unsigned long kasan_zero_pte[PTRS_PER_PTE] __page_aligned_bss;
static unsigned long kasan_zero_pmd[PTRS_PER_PMD] __page_aligned_bss;
static unsigned long kasan_zero_pud[PTRS_PER_PUD] __page_aligned_bss;

// A read-only page of all zeros mapped as shadow:
// Any shadow access returns 0x00 = "memory valid" during early boot
// This suppresses false positives during early initialization
```

Once `kasan_init()` runs (much later), it:
1. Allocates real shadow pages for each physical memory region
2. Initializes shadow with proper initial values (0x00 for free pages)
3. Installs redzones around existing allocations

---

## KASAN Compiler Instrumentation Example

```c
// Source code:
void copy_data(char *dst, char *src, int n) {
    for (int i = 0; i < n; i++)
        dst[i] = src[i];
}

// Compiler-generated pseudo-code with KASAN:
void copy_data(char *dst, char *src, int n) {
    for (int i = 0; i < n; i++) {
        // Compiler-inserted KASAN check for src[i]:
        __kasan_check_read(src + i, 1);
        // Compiler-inserted KASAN check for dst[i]:
        __kasan_check_write(dst + i, 1);
        dst[i] = src[i];
    }
}

// __kasan_check_read expands to:
static __always_inline void __kasan_check_read(const void *p, unsigned int size) {
    u8 shadow = *(u8 *)kasan_mem_to_shadow(p);
    if (shadow != 0) {
        // Shadow is non-zero: potential invalid access
        if (size > 8 || ((long)p & 7) + size > shadow)
            kasan_report(p, size, false, __RET_IP__);
    }
}
```

The compiler inserts these checks at every memory access. The early `kasan_early_init`
ensures these checks don't crash during the early boot phase.

---

## Memory Overhead Analysis

For a system with 16 GB RAM:
```
RAM size:             16 GB = 16 × 1024³ bytes = 17,179,869,184 bytes
KASAN shadow size:    16 GB / 8 = 2 GB
Early boot shadow:    Only kernel image range ≈ 10-50 MB (mapped to zero page)
Full shadow:          2 GB (after kasan_init)
```

This 2 GB shadow overhead is why KASAN is only for development/debugging builds,
not production kernels. On servers with 1 TB RAM, KASAN would need 128 GB shadow.

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