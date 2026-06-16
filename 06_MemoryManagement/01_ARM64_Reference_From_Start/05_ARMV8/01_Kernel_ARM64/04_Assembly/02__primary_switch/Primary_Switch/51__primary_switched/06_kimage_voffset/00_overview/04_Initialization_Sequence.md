# `kimage_voffset` Initialization — Step by Step

## Context Before `__primary_switched`

By the time `__primary_switched` executes:

1. **`primary_entry`** — Entry from firmware
   - `x0` = FDT physical address (saved to x21)
   - `bl preserve_boot_args` → saves x0-x3 to `boot_args[]`
   - `bl el2_setup` or `init_kernel_el` → configure EL, return to EL1

2. **`__primary_switch`** — MMU enable sequence  
   - `bl __cpu_setup` → sets MAIR, TCR, SCTLR (except MMU enable bit)
   - `bl __enable_mmu` → enables MMU via SCTLR_EL1.M=1, returns via VA
   - At return from `__enable_mmu`: CPU is executing at virtual addresses

3. **After MMU enable** — Before `__primary_switched`
   - `x0` = PA of `_text` (from the MMU setup code path)
   - `x21` = FDT PA
   - `x20` = boot mode (EL1 or EL2)
   - PC = virtual address (in the new kernel VA space)

---

## The `__primary_switched` Sequence (Annotated)

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    /*
     * sp_el0 = &init_task   (current = init_task)
     * sp = init_stack top - PT_REGS_SIZE  (kernel stack)
     * x29 = sentinel frame pointer
     * tpidr_el1 = per-CPU offset for CPU 0
     */
    adr_l   x4, init_task          // x4 = &init_task VA
    init_cpu_task x4, x5, x6       // sets up sp, sp_el0, tpidr_el1, SCS

    /* Install exception vectors */
    adr_l   x8, vectors
    msr     vbar_el1, x8
    isb

    /* Create C stack frame */
    stp     x29, x30, [sp, #-16]!  // push frame
    mov     x29, sp

    /* Save FDT pointer */
    str_l   x21, __fdt_pointer, x5  // __fdt_pointer = FDT PA

    /*
     * kimage_voffset = VA(_text) - PA(_text)
     * x0 = PA(_text)   (from the boot path that set up MMU)
     */
    adrp    x4, _text               // x4 = VA(_text) — COMPUTED HERE
    sub     x4, x4, x0              // x4 = VA - PA = kimage_voffset
    str_l   x4, kimage_voffset, x5  // kimage_voffset = x4 — STORED HERE

    /* Other boot setup... */
    bl      set_cpu_boot_mode_flag
#ifdef CONFIG_KASAN
    bl      kasan_early_init
#endif
    mov     x0, x20                 // x0 = boot mode for finalise_el2
    bl      finalise_el2

    /* Transfer to C runtime */
    bl      start_kernel
    ASM_BUG()                       // should never reach here
SYM_FUNC_END(__primary_switched)
```

---

## The Critical Window: Between `__enable_mmu` and `str_l kimage_voffset`

During this window:
- MMU is ON → CPU uses VA
- `kimage_voffset` is NOT yet set
- Any C code that calls `__pa()` would use uninitialized `kimage_voffset`

This is SAFE because:
- No C code runs in this window (we're in assembly)
- `init_cpu_task` is a pure assembly macro
- `adr_l`, `msr`, `isb` are pure assembly
- `stp`, `mov` are pure assembly
- `str_l x21, __fdt_pointer` is pure assembly

The first C function call is `start_kernel`, and by then `kimage_voffset` is set.

---

## What Happens at Secondary CPU Startup

Secondary CPUs go through `secondary_startup` → `secondary_switched`, which does:
```asm
__secondary_switched:
    ...
    adr_l   x7, kimage_voffset      // load kimage_voffset
    ldr     x4, [x7]                // x4 = kimage_voffset (already set by primary CPU)
    // Secondary CPUs DON'T compute kimage_voffset — they use the primary CPU's value
```

`kimage_voffset` is set ONCE by the primary CPU in `__primary_switched`.
All subsequent CPUs just read the already-computed value. This is safe because:
1. Secondary CPUs don't boot until long after `start_kernel` is called
2. `kimage_voffset` is already stable (and has been made `__ro_after_init`)
3. The value is the SAME for all CPUs (same kernel image, same VA-PA mapping)

---

## Verification After Setup

After boot, you can verify `kimage_voffset`:

```bash
# Via /proc/kallsyms (KASLR-aware symbol addresses):
grep ' T _text' /proc/kallsyms
# Example: ffff800010000000 T _text

# Via /proc/iomem (physical address of kernel):
grep 'Kernel code' /proc/iomem
# Example: 40200000-41ffffff : Kernel code

# kimage_voffset = VA - PA:
# 0xffff800010000000 - 0x40200000 = 0xffff7fffd0000000
# (This is the ACTUAL value on that specific system)

# Via crash(8) for kdump analysis:
crash> p kimage_voffset
kimage_voffset = $1 = -140733193441280   # signed decimal = 0xffff800000000000 unsigned
```

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