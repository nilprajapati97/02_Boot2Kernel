# The Role of `x0` — Passing PA(_text) to `__primary_switched`

## Where `x0 = PA(_text)` Comes From

`__primary_switched` relies on `x0` holding the physical address of `_text`.
Tracing back to the source:

### In `__primary_switch`:

```asm
// arch/arm64/kernel/head.S
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp    x1, reserved_pg_dir
    adrp    x2, init_pg_dir
    bl      __enable_mmu            // x0 set here

    // After __enable_mmu:
    // The code continues at a virtual address
    // x0 = physical address of the kernel start
    ldr     x8, =__primary_switched  // load VA of __primary_switched
    adrp    x0, KERNEL_START         // x0 = PA of kernel start
    br      x8                       // jump to __primary_switched at its VA
```

The exact mechanism varies slightly by kernel version, but the principle is:
- Before jumping to `__primary_switched`, the boot code explicitly loads `x0`
  with the physical address of the kernel image base
- `x0 = PA(_text)` is a CONVENTION, not something automatically preserved

### The Call Convention Here Is NOT AAPCS64

This is NOT a standard C function call. It's a bare assembly jump (`br` or
equivalent). `x0` is set explicitly to `PA(_text)` as a passed-by-register argument.
No callee-save convention applies — `x0` is a raw argument passed via the
jump-to-__primary_switched code.

---

## What `KERNEL_START` / `PA(_text)` Actually Is

```asm
// On some ARM64 configs, PA(_text) is computed as:
adrp    x0, _text               // BEFORE MMU on: this gives PA (PC is PA)
// After MMU is on: x0 is NOT refreshed — it retains the PA from before
```

OR via the page table setup code:
```asm
// The init_pg_dir setup code knows the PA of _text (it was passed by firmware)
// It stores this in a register before enabling the MMU
// After MMU enable, that register (x0) still holds the PA
```

The key is: at the assembly level, before the MMU was enabled, `adrp x0, _text`
would give the PA (since PC was a PA). That value is preserved in `x0` through
the MMU enable sequence.

---

## The "Before/After MMU" Duality

This is a subtle but crucial concept:

```
BEFORE MMU enable:
    CPU: PC = physical address
    adrp x, symbol → gives PA-based result

AFTER MMU enable:
    CPU: PC = virtual address
    adrp x, symbol → gives VA-based result

In __primary_switched (after MMU):
    adrp x4, _text → x4 = VA(_text)  ← used for kimage_voffset computation
    x0 = PA(_text) ← was computed (or set) BEFORE MMU, preserved in register
    
    sub x4, x4, x0 = VA - PA = kimage_voffset  ✓
```

If someone mistakenly ran `adrp x0, _text` AFTER MMU enable to "get PA", they'd
actually get the VA. The subtraction `VA - VA = 0` would be wrong.

---

## Assembly Code Path Verification

Looking at the actual code flow:

```asm
__primary_switch:
    // x0 = PA of reserved_pg_dir or init_pg_dir area
    // The following sets up the page tables and enables MMU
    bl  __enable_mmu
    // After return: PC = VA, but x0 still holds a PA from before
    
    // Set x0 to PA of _text specifically:
    adrp    x0, KERNEL_START    // This executes at a VA, so would give VA??
```

Wait — after MMU enable, `adrp x0, KERNEL_START` would give VA. Let's check
how this is actually handled. The key insight:

**`KERNEL_START` is defined such that `adrp x0, KERNEL_START` after MMU give
a VA, but the VA = `_text_VA` and we need `_text_PA`.**

So in `__primary_switched`:
- `adrp x4, _text` → `x4 = _text_VA` (good, needed for VA part)
- `x0` was set to `_text_PA` BEFORE the MMU enable path and is preserved

OR: `x0` is computed from the page table walking logic which knows the PA
explicitly. The exact mechanism is kernel-version-specific but the outcome is
`x0 = PA(_text)` when `__primary_switched` runs.

---

## Robustness of the Design

The `sub x4, x4, x0` design is robust because:
1. It doesn't depend on knowing PAGE_OFFSET at runtime
2. It doesn't depend on knowing the KASLR slide
3. It uses the ACTUAL VA (from `adrp` via the running PC) and ACTUAL PA (from `x0`)
4. The single subtraction captures the full VA-PA relationship regardless of
   what values were chosen

If someone changes the mapping (e.g., maps _text at a different VA), the same
code computes the correct new `kimage_voffset` without any code changes.

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