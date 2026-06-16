# KASLR ARM64 — Complete Internals

**Feature:** Kernel Address Space Layout Randomization  
**Source:** `arch/arm64/kernel/kaslr.c`, `arch/arm64/mm/mmu.c`  
**Config:** `CONFIG_RANDOMIZE_BASE`

---

## 0. What KASLR Randomizes

Without KASLR:
```
Kernel always loads at a fixed PA:    0x8000_0000 (or another fixed address)
Kernel VA always starts at:           0xFFFF_8000_0000_0000 (or fixed VA_START)
kimage_voffset = fixed value          (known by attackers)
```

With KASLR:
```
Kernel PA:       randomized by bootloader   (e.g., 0x5A20_0000)
Kernel VA:       PA + random offset         (e.g., 0xFFFF_DE49_2A20_0000)
kimage_voffset:  VA(_text) - PA(_text)      = unknown per boot
```

An attacker who cannot determine the kernel VA cannot use hardcoded gadget
addresses for ROP chains. KASLR makes exploitation significantly harder.

---

## 1. Two Kinds of KASLR Randomization

ARM64 KASLR has two independent randomizations:

### Type 1: Physical Load Address Randomization

The bootloader (GRUB, EDK2, etc.) chooses a random physical address to load
the kernel:

```
PA_base = DRAM_base + random_offset_2MB_aligned
```

The `random_offset` is chosen from entropy sources available to the bootloader
(EFI random number generator, RNDR instruction, etc.).

The kernel code handles this by:
1. Linking with a fixed VA start (compile-time constant)
2. Computing `kimage_voffset = _text_VA - _text_PA` at runtime
3. Using `kimage_voffset` for all PA↔VA conversions

### Type 2: Virtual Address Offset Randomization

Even if the PA is known (e.g., on systems without bootloader randomization),
the VA can be additionally randomized:

```c
// arch/arm64/kernel/kaslr.c
void __init kaslr_init(void)
{
    u64 seed;
    
    if (!kaslr_enabled())
        return;
    
    // Get random seed:
    seed = get_random_u64();  // From RNDR/bootloader/EFI RNG
    
    // Compute VA offset:
    module_range = MODULES_END - MODULES_START;
    va_offset = seed & module_range_mask;
    va_offset = ALIGN(va_offset, MODULE_ALIGN);
    
    // Apply: shift the entire kernel VA space
    kimage_vaddr += va_offset;
    kimage_voffset -= va_offset;
}
```

This shifts the kernel VA relative to its linked address by a random amount
within the allowed range.

---

## 2. Memory Regions and Their Randomization

| Region | Size | Randomized? |
|---|---|---|
| `vmalloc` | 128TB | Yes (offset from VA_START) |
| `modules` | 128MB | Yes (same offset) |
| kernel `.text` | varies | Yes (physical + VA offset) |
| `vmemmap` | 4TB | Yes |
| `PAGE_OFFSET` (linear map) | 128TB | **No** (not randomized) |

The linear map (direct physical memory mapping) is NOT randomized in standard
Linux ARM64 KASLR. The randomization is for the kernel image VA, not the
physical memory linear map.

---

## 3. `kimage_voffset` — The Central KASLR Variable

```c
// arch/arm64/include/asm/memory.h — line 247
extern u64 kimage_voffset;

// How it is set:
// kimage_voffset = VA of _text - PA of _text
// = compile-time VA of _text - runtime PA of _text
```

**All PA↔VA conversions for the kernel image go through `kimage_voffset`:**

```c
// include/asm/memory.h — line 341
#define __kimg_to_phys(addr)   ((addr) - kimage_voffset)
#define __phys_to_kimg(phys)   ((phys) + kimage_voffset)
```

For example, to find the physical address of a kernel symbol:
```c
void *sym_va = (void *)&some_kernel_function;
phys_addr_t sym_pa = __kimg_to_phys((unsigned long)sym_va);
// sym_pa = sym_va - kimage_voffset
```

---

## 4. How `kimage_voffset` Is Computed During Boot

The computation happens in `__pi_early_map_kernel`:

```c
// In map_kernel.c (PI context, pre-MMU):

// _text is the compile-time VA of the kernel text start
// __pa_symbol(_text) computes the PA using PC-relative arithmetic

unsigned long text_va = (unsigned long)&_text;       // Compile-time VA
unsigned long text_pa = (unsigned long)__pa_symbol(_text);  // Runtime PA

// Before MMU, text_pa is computed as:
// text_pa = (PC & page_mask) + offset_of_text_from_PC
// This gives the actual physical load address

kimage_voffset_value = text_va - text_pa;

// Store to both PI and non-PI versions:
*(unsigned long *)__pi_kimage_voffset = kimage_voffset_value;
// __pi_kimage_voffset = PI-accessible (pre-MMU)
// kimage_voffset = normal (post-MMU)
```

---

## 5. The `__pa_symbol` Macro — Position-Independent PA Computation

```c
// include/asm/memory.h
#define __pa_symbol(x)                                              \
    __phys_addr_symbol(RELOC_HIDE((unsigned long)(x), 0))

// For PI (position-independent) builds, this becomes:
// PA of symbol = &symbol - (VA of _text - PA of _text)
// But before MMU: PA is computed via PC-relative load address:
// PA of symbol = __pa_symbol uses adrp/add arithmetic in assembly
```

In the PI context (`__pi_` prefix functions compiled with `-fPIC`):
```asm
adrp    x0, _text                 // x0 = PA of _text page (PC-relative)
add     x0, x0, :lo12:_text       // x0 = exact PA of _text
// This gives the RUNTIME physical address regardless of KASLR
```

---

## 6. KASLR and the Identity Map

The identity map must cover the KASLR-randomized physical load address:

```c
// In __pi_create_init_idmap, called from __pi_early_map_kernel:
// pa_start = runtime PA of _text (KASLR-aware)
// pa_end = runtime PA of _end

// Creates identity mapping: VA[pa_start, pa_end) → PA[pa_start, pa_end)
// This is correct regardless of KASLR because pa_start is computed at runtime
```

If the identity map used a hardcoded PA (e.g., always 0x4000_0000), KASLR
would break it. The PI code avoids this by using PC-relative address computation.

---

## 7. KASLR Entropy Sources on ARM64

```c
// arch/arm64/kernel/kaslr.c
static u64 get_kaslr_seed(struct boot_params *params)
{
    u64 seed = 0;
    
    // Source 1: Bootloader via FDT /chosen/kaslr-seed
    seed = of_get_flat_dt_prop(chosen, "kaslr-seed", NULL);
    
    // Source 2: EFI random number generator (UEFI RNG protocol)
    efi_get_random_bytes(sizeof(seed), (u8 *)&seed);
    
    // Source 3: Hardware RNDR instruction (ARMv8.5 FEAT_RNG)
    if (cpu_have_feature(cpu_feature(RNG))) {
        unsigned long rndr;
        asm volatile("mrs %0, s3_3_c2_c4_0" : "=r"(rndr));
        seed ^= rndr;
    }
    
    return seed;
}
```

The seed is XOR'd with multiple sources to prevent any single source compromise
from predicting the KASLR offset.

---

## 8. KASLR Security Analysis

### What KASLR Mitigates
- **ROP/JOP attacks:** Gadget addresses are unknown
- **Data-oriented attacks:** Kernel data structure addresses are unknown
- **Blind kernel exploits:** Cannot assume fixed kernel layout

### What KASLR Does NOT Mitigate
- **Side-channel attacks:** Cache timing, speculative execution can leak addresses
  (see Spectre, Meltdown)
- **Kernel infoleak vulnerabilities:** If the kernel leaks its own addresses
  (e.g., via `/proc/kallsyms` when `kptr_restrict=0`), KASLR is bypassed
- **Physical memory attacks:** DMA attacks, hardware probing (KASLR randomizes VA,
  not PA — but PA randomization is also done by bootloader on KASLR kernels)
- **Brute force (32-bit VA systems):** Only relevant where VA space is small

### Defense in Depth
KASLR is NOT a standalone mitigation. It is most effective combined with:
1. `CONFIG_RANDOMIZE_BASE` (KASLR itself)
2. `CONFIG_STRICT_KERNEL_RWX` (non-writable kernel text)
3. `CONFIG_STACKPROTECTOR` (stack canaries)
4. `CONFIG_ARM64_PTR_AUTH` (PAC — pointer authentication)
5. `CONFIG_ARM64_BTI` (Branch Target Identification)
6. KPTI (Meltdown mitigation, not needed on VHE systems)

---

## 9. Disabling KASLR

For debugging:
```
# Kernel command line:
nokaslr

# Or build without:
# CONFIG_RANDOMIZE_BASE is not set
```

With `nokaslr`, `kimage_voffset = _text_VA - _text_PA` where `_text_PA` is
the fixed bootloader load address. This is typically a negative number with
the high bits set (since kernel VA > kernel PA).

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASLR (Kernel Address Space Layout Randomization) on ARMv8-A is implemented by choosing a random physical load address (phys_offset) and a random virtual mapping offset (kimage_voffset) at boot time. The CPU does not know or care about randomization: it simply uses whatever address is in TTBR1_EL1 as the root of the kernel page table. The EL1 exception vector base (VBAR_EL1) is also randomized as a consequence of the kernel image being loaded at a random VA. The hardware has no KASLR-awareness.

### Kernel Perspective (Linux ARM64)
KASLR is implemented in __pi_kaslr_early_init (arch/arm64/kernel/pi/kaslr_early.c). It uses the EFI random number generator (or RNDR instruction on ARMv8.5+) to pick a random offset that is aligned to the minimum KASLR granularity (2 MB for 4 KB pages). The offset is applied by:
  1. Choosing a random VA base for the kernel image within the TTBR1_EL1 region.
  2. Updating kimage_voffset = VA - PA.
  3. Updating all ELF RELA relocation entries to reflect the new VA.
  4. Flushing the D-cache for all modified sections.

### Memory Perspective (ARMv8 Memory Model)
KASLR changes the kernel's VA layout but not its PA layout (for a given boot). The TTBR1_EL1 page tables point to the randomized kernel VA. kimage_voffset is a compile-time-unknown runtime constant used to convert between kernel PA and kernel VA: VA = PA + kimage_voffset. Because the kernel uses position-independent code (__pi_ prefix) during the relocation phase, all accesses are PA-relative until the relocations are applied. After __primary_switch, all kernel code runs at the final randomized VA.