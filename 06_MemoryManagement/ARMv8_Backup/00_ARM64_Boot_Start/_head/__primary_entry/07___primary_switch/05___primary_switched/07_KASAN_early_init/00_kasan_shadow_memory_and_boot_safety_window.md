# KASAN Early Init — Shadow Memory Map, Instrumentation Activation, and Boot Safety Window

**File**: `arch/arm64/kernel/head.S` — inside `__primary_switched`
**Instructions**:
```asm
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
    bl      kasan_early_init
#endif
```
**Implementation**: `arch/arm64/mm/kasan_init.c`
**Perspective**: Memory Safety Architecture / Address Sanitizer Design
**Style**: NVIDIA Security Engineering / Google AddressSanitizer Design Document

---

## 1. What Is KASAN and Why It Needs Early Init

KASAN (Kernel Address SANitizer) detects memory safety bugs at runtime:
- **Use-after-free**: access to freed memory
- **Out-of-bounds**: array overrun, buffer overflow
- **Stack buffer overflow**: local variable overrun
- **Global buffer overflow**: bss/data section overrun

KASAN implements detection by using a **shadow memory region**:
- Every 8 bytes of kernel memory has 1 byte of shadow state
- Shadow byte = 0 → all 8 bytes addressable
- Shadow byte = 1..7 → only first N bytes addressable
- Shadow byte < 0 → memory is poisoned (freed, out-of-bounds, etc.)

```
Kernel VA space with KASAN:
  ┌────────────────────────────────────┐ 0xFFFF_FFFF_FFFF_FFFF
  │   Kernel text, data, stacks, heap  │
  │   EVERY byte access here           │
  │   → compiler inserts shadow check  │
  │   → shadow byte at KASAN_SHADOW(addr) checked  │
  ├────────────────────────────────────┤
  │   KASAN Shadow Region              │ ~1/8 of kernel VA space
  │   1 byte shadows 8 kernel bytes    │
  └────────────────────────────────────┘ 0xFFFF_0000_0000_0000 (approx)
```

---

## 2. The Two KASAN Variants and Their Shadow Granularity

```
CONFIG_KASAN_GENERIC:
  - Software implementation, no hardware support needed
  - Shadow granularity: 8 bytes → 1 shadow byte  (1:8 ratio)
  - Full tracking: heap, stack, globals, vmalloc
  - Performance overhead: ~2x memory usage, ~2x slowdown
  - Used in development/CI/fuzzing builds

CONFIG_KASAN_SW_TAGS:
  - ARM64-specific, uses top-byte-ignore (TBI) feature
  - Tags 8 random bits in pointer's top byte
  - Shadow maps tags: 1 byte shadow per 16 bytes kernel memory
  - Lower overhead than GENERIC: ~15% memory, ~20% slowdown
  - Can detect temporal safety bugs (use-after-free via tag mismatch)
  - Used on ARM64 platforms that enable TBI (Tegra, Snapdragon, etc.)

CONFIG_KASAN_HW_TAGS:
  - Uses ARM MTE (Memory Tagging Extension) hardware
  - No early init needed (hardware handles tagging, not this path)
  - NOT covered by this guard: #if GENERIC || SW_TAGS
```

---

## 3. What `kasan_early_init` Does

```c
// arch/arm64/mm/kasan_init.c
void __init kasan_early_init(void)
{
    BUILD_BUG_ON(KASAN_SHADOW_OFFSET !=
        KASAN_SHADOW_END - (1UL << (64 - KASAN_SHADOW_SCALE_SHIFT)));

    // Install early shadow page tables:
    // Map the entire KASAN shadow region to a single "zero page"
    // called kasan_early_shadow_page.
    //
    // This page has all bytes = 0 (no poisoning) — means every
    // kernel address is "valid" from KASAN's perspective during early boot.
    //
    // This is necessary because the compiler has already instrumented
    // __primary_switched and everything it calls. Every function call
    // will generate a shadow memory access. Without this mapping, the
    // shadow access would fault on an unmapped address.

    kasan_map_early_shadow(swapper_pg_dir);
}
```

The key operation: **map the shadow region** (a large VA range, ~1/8 of VA)
to a single read-only zero page. Every shadow byte read returns 0 = "valid".
Every shadow byte write goes to... a zero page that ignores writes
(actually a writable page that gets overwritten once KASAN is fully inited).

---

## 4. The Instrumentation Window Problem

After `__primary_switched` sets up VBAR_EL1 and the stack, **every function
call generates KASAN shadow memory accesses**. The compiler (with
`-fsanitize=kernel-address`) inserts checks like:

```c
// Conceptual compiler-inserted code for: int x = buf[i];
void *shadow = (void *)((unsigned long)&buf[i] >> 3) + KASAN_SHADOW_OFFSET;
unsigned char shadow_byte = *shadow;  // ← THIS DEREFERENCES SHADOW VA
if (shadow_byte != 0) {
    kasan_report(&buf[i], 1, false, _RET_IP_);
}
int x = buf[i];
```

Without `kasan_early_init`, the `*shadow` dereference would:
1. Try to access an address in the KASAN shadow region
2. Find no page table entry (shadow not mapped yet)
3. Trigger a translation fault
4. Exception handler invoked (VBAR_EL1 is valid by now)
5. Kernel panic: "Unable to handle kernel paging request"
6. Boot fails

**The placement of `kasan_early_init` here — after VBAR_EL1 and stack,
before any C function calls — is the exact minimum viable window.**

---

## 5. Ordering Rationale: Why Here, Not Earlier or Later

```
__primary_switched execution order:
  init_cpu_task      — sets up stack (SP is now valid)
  vbar_el1           — exception vectors installed (faults now handled)
  stp/mov            — calling frame established
  str_l __fdt_pointer — trivial store, no function call, no shadow access
  adrp/sub kimage_voffset — trivial arithmetic + store, no shadow
  set_cpu_boot_mode_flag — bl (function call!) ← shadow access HAPPENS HERE
  [KASAN early init here] ← must be before set_cpu_boot_mode_flag?

Actually: set_cpu_boot_mode_flag is assembly, may not be instrumented.
But finalise_el2 calls C code and bl start_kernel is heavily C.

The actual boundary: kasan_early_init is placed BEFORE finalise_el2 and
start_kernel because those are C functions that WILL have shadow accesses.
The assembly functions before (set_cpu_boot_mode_flag, str_l macros) are
written to avoid KASAN instrumentation issues.
```

```
Dependency:
  MUST have:  Stack (SP valid)       ← provided by init_cpu_task
  MUST have:  MMU ON + swapper_pg_dir ← provided by __primary_switch (before br x8)
  MUST have:  Exception vectors      ← provided by vbar_el1 install
  NEEDED by:  kasan_map_early_shadow(swapper_pg_dir) — modifies page tables
  NEEDED for: All subsequent C function calls (instrumented)
```

---

## 6. Early Shadow vs Full Shadow

`kasan_early_init` creates a **stub shadow** — all zeros, all valid.
This means KASAN is **not yet detecting bugs** during early boot.

Full KASAN initialization happens later:
```c
start_kernel
  → setup_arch
    → kasan_init()      // arch/arm64/mm/kasan_init.c
```

`kasan_init()` rebuilds the shadow page tables to properly track each
kernel page's poison state, sets up the kmalloc allocator integration,
and enables real detection.

```
KASAN detection capability timeline:
  [Before kasan_early_init]     No detection (would fault)
  [After kasan_early_init]      No detection (all shadow = 0 = valid)
  [After kasan_init()]          Full detection: heap, globals, stack
```

The early init is purely about **survivability**, not detection.

---

## 7. Memory Cost at Early Init

On a 4GB kernel virtual address space (ARM64 with 39-bit VA):
```
KASAN_SHADOW_SIZE = kernel_va_size / 8 = 512MB / 8 = 64MB shadow needed

kasan_early_init: map 64MB VA range → 1 physical page (zero page)
  Page table cost: ~8KB of early page tables (4-level walk)
  Physical memory cost: 1 x 4KB page (kasan_early_shadow_page)

After kasan_init():
  Each kernel page gets its own shadow page
  Total physical cost: n_kernel_pages / 8 shadow pages
  For a typical kernel: ~2MB kernel text → 256KB shadow memory
```

The 1-page trick during `kasan_early_init` avoids allocating full shadow
backing before the memory allocator is ready — a clean bootstrapping solution.
