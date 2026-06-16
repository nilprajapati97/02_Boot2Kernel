# `kimage_voffset` — Preview and Importance

**Symbol:** `kimage_voffset` in `arch/arm64/include/asm/memory.h`  
**Context:** How it is set during boot, what it encodes, and why it is critical

---

## 0. What Is `kimage_voffset`?

`kimage_voffset` is a global kernel variable that stores the difference between
the **virtual address** at which the kernel actually runs and the **physical
address** at which it is loaded:

```
kimage_voffset = VA(_text) - PA(_text)
               = (KIMAGE_VADDR + KASLR_VA_offset) - (load_PA)
```

This is the key constant for translating between kernel VAs and PAs.

---

## 1. Definition

```c
// arch/arm64/include/asm/memory.h (approximately line 247)
extern s64 kimage_voffset;

// Used by __pa() and __va() macros:
#define __pa(x)     ((phys_addr_t)((x) - kimage_voffset))
#define __va(x)     ((void *)((unsigned long)(x) + kimage_voffset))
```

Note: `kimage_voffset` is of type `s64` (signed 64-bit integer). It can be
negative (if the VA base is lower than expected). In practice, for a 48-bit VA
kernel, it is always a large positive number (~`0xFFFF_8000_8000_0000`).

---

## 2. How It Is Set During Boot

`kimage_voffset` is set by `__pi_early_map_kernel` (which runs before MMU
enable, using PI code):

```c
// arch/arm64/mm/init.c or arch/arm64/kernel/pi/map_kernel.c (simplified):
void __init __pi_early_map_kernel(...)
{
    unsigned long text_pa = (unsigned long)__pa_symbol(_text);
    // __pa_symbol uses __pi_ PI code to get PA:
    //   text_pa = (unsigned long)_text - __pi_kimage_voffset
    
    unsigned long kaslr_offset = ...;  // Determined from FDT/RNDR
    
    unsigned long text_va = (unsigned long)_text + kaslr_offset;
    
    kimage_voffset = text_va - text_pa;
    // = (KIMAGE_VADDR + kaslr_offset) - load_PA
}
```

**In Practice (assembly perspective):**

At the end of `__pi_early_map_kernel`, the kernel stores `kimage_voffset` to
the memory location of the `kimage_voffset` global variable using a PA-relative
write (since MMU is off).

The PI version is `__pi_kimage_voffset`. When the MMU is enabled and the kernel
runs at its virtual addresses, `kimage_voffset` (the non-PI version) is at the
VA `&kimage_voffset`. Both contain the same value; they just reside at different
addresses (PA vs. VA).

---

## 3. The `__pa()` and `__va()` Macros in Detail

Once `kimage_voffset` is set:

```c
// Convert a kernel VA to a PA (for kernel image symbols):
#define __pa(x)     ((phys_addr_t)((unsigned long)(x) - kimage_voffset))

// Example:
PA(start_kernel) = VA(start_kernel) - kimage_voffset
                 = 0xFFFF_8000_9234_1234 - 0xFFFF_8000_8000_0000
                 = 0x0000_0000_1234_1234
                 (= offset from _text + load PA)
```

```c
// Convert a PA to a kernel VA (for kernel image symbols):
#define __va(x)     ((void *)((unsigned long)(x) + kimage_voffset))

// Example:
VA(0x1234_1234) = 0x1234_1234 + 0xFFFF_8000_8000_0000
                = 0xFFFF_8000_9234_1234
```

These macros work for any address in the kernel binary (between `_text` and
`_end`). They do NOT work for:
- Arbitrary PA (e.g., device registers, other processes' memory)
- Linear map range (use `__phys_to_virt`/`phys_to_virt` instead)

---

## 4. `kimage_voffset` vs Linear Map Offset (`PAGE_OFFSET`)

Two different offset concepts:

| Offset | Formula | Used For |
|---|---|---|
| `kimage_voffset` | `VA(_text) - PA(_text)` | Kernel image PA↔VA |
| `PAGE_OFFSET` | `phys_to_virt(0) = PAGE_OFFSET` | Linear map PA↔VA |

The linear map and the kernel image are in different VA regions:
- Linear map: VA around `0xFFFF_0000_0000_0000` (lower kernel half)
- Kernel image: VA around `0xFFFF_8000_xxxx_xxxx` (upper kernel half)

The two offsets are different and not interchangeable.

---

## 5. Security: `kimage_voffset` and KASLR

`kimage_voffset` **reveals the KASLR offset**. An attacker who can read
`kimage_voffset` can compute where the kernel is loaded:

```
KASLR_offset = kimage_voffset - (KIMAGE_VADDR - default_load_PA)
```

This is why:
1. `kimage_voffset` is NOT exported to user space (`/proc/kallsyms` is root-only)
2. `KASLR_offset` is zeroized from registers as soon as it's stored
3. Memory access to kernel globals from user space requires privilege (MPU/PAN)

---

## 6. PI Duplicate: `__pi_kimage_voffset`

Before the MMU is enabled, code cannot access `kimage_voffset` at its VA.
Instead, the PI version `__pi_kimage_voffset` is accessed at its PA:

```c
// arch/arm64/kernel/pi/map_kernel.c
// These are the same variable — different symbols for pre/post-MMU access:
extern u64 kimage_voffset;        // VA access (post-MMU)
extern u64 __pi_kimage_voffset;   // PA access (pre-MMU, via PI naming convention)
```

The `__pi_kimage_voffset` symbol is accessed via PC-relative code (e.g.,
`adrp`+`ldr` which computes a PA-based address since PC is a PA before MMU).
This correctly reads the PI-accessible copy.

---

## 7. Timeline: When `kimage_voffset` Becomes Available

| Boot Phase | `kimage_voffset` State |
|---|---|
| Before `__primary_switch` | Not set (0 or garbage) |
| During `__pi_early_map_kernel` | Being computed and stored to PA |
| After `__pi_early_map_kernel` (pre-MMU) | `__pi_kimage_voffset` is valid |
| After `__enable_mmu` (VA mode) | `kimage_voffset` VA is valid |
| In `__primary_switched` and beyond | Fully usable in `__pa`/`__va` macros |

Any code that calls `__pa(sym)` or `__va(pa)` before `__primary_switched` would
read a zero or garbage `kimage_voffset` and produce a wrong address.

---

## 8. How Secondary CPUs Use `kimage_voffset`

Secondary CPUs (CPU1, CPU2...) do not run `__pi_early_map_kernel`. Instead,
they skip to `secondary_startup` which eventually calls `secondary_entry`,
which reads the pre-set `kimage_voffset` from memory (already set by the
primary CPU).

This works because:
1. Primary CPU sets `kimage_voffset`
2. Memory is coherent (caches initialized, multi-CPU memory model applies)
3. Secondary CPUs just load `kimage_voffset` and proceed to the VA world

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The ARM64 kernel virtual memory map uses the upper half of the 64-bit VA space (addresses with the top T1SZ bits = 1, i.e., 0xFFFF_xxxx_xxxx_xxxx for 48-bit). TTBR1_EL1 translates these addresses. The layout from high to low is:
- 0xFFFF_FFFF_FFFF_FFFF: vmalloc region top
- kernel text/data/bss: mapped by kimage_voffset + PA
- linear map: VA = PAGE_OFFSET + PA (direct-map of all physical RAM)
- vmalloc / vmap area
- PCI I/O space / fixmap
The hardware only cares about TTBR1_EL1 root and TCR_EL1 T1SZ. All the regions above are software conventions; the CPU treats them uniformly via the page tables.

### Kernel Perspective (Linux ARM64)
kimage_voffset = (kernel_VA_start - kernel_PA_start). After KASLR, kimage_voffset is set at boot and used by:
- __phys_to_virt(pa): va = pa - PHYS_OFFSET + PAGE_OFFSET
- __virt_to_phys(va): pa = va - kimage_voffset
The kernel linear map is set up in map_kernel and map_mem (arch/arm64/mm/mmu.c). The kernel text is mapped read-only/execute, the data is read-write/no-execute. After start_kernel, paging_init() rebuilds the definitive page tables.

### Memory Perspective (ARMv8 Memory Model)
The virtual memory map is purely a software abstraction enforced by the page tables. Physically, the linear map means every byte of physical RAM has a corresponding kernel VA: VA = PAGE_OFFSET + PA. This allows the kernel to access any physical address by simple arithmetic. The kimage offset separates the kernel text/data from the linear map to allow different permissions: kernel text is mapped Execute (PXN=0) but not writable; linear map is mapped Read-Write (AP=0b01) but not executable (PXN=1, UXN=1). Both regions use Normal Inner-Shareable Write-Back Cacheable attributes (MT_NORMAL).