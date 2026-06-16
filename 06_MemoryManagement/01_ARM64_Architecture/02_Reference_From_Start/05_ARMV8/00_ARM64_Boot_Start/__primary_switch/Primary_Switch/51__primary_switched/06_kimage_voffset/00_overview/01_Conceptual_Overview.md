# `kimage_voffset` — Conceptual Overview

## The Problem: Kernel Doesn't Know Its Own Load Address

When the Linux ARM64 kernel binary is compiled, the linker assigns virtual
addresses to all symbols assuming a fixed load address (e.g., `_text = 0xffff800010000000`).
These addresses are compiled into the kernel as constants.

At runtime, the bootloader may place the kernel at a DIFFERENT physical address
(due to KASLR or firmware constraints). The virtual address the CPU uses depends
on the page tables the kernel itself sets up.

The mismatch:

```
Compile time: _text VA = 0xffff800010000000 (linker script constant)
Runtime PA:   _text PA = 0x78000000 (bootloader choice, may vary with KASLR)
Runtime VA:   _text VA = 0xffff800010000000 + kaslr_va_offset (page table)
```

`kimage_voffset = runtime VA - runtime PA`

---

## Three Architectural Layers

```
Layer 1: PHYSICAL ADDRESSES (PA)
──────────────────────────────────────────────────────────
What the hardware "really" uses. RAM chips are identified
by PA. DMA devices use PA. Page table entries contain PA.
┌──────────────────────────────────────────────────────┐
│  0x00000000 - 0x3FFFFFFF: Device memory (MMIO)       │
│  0x40000000 - 0x7FFFFFFF: RAM (1 GB example)         │
│    0x40200000: _text physical (kernel image start)    │
│    0x48000000: FDT blob                               │
│    0x50000000: initrd                                 │
└──────────────────────────────────────────────────────┘

Layer 2: VIRTUAL ADDRESSES (VA) — Kernel Perspective
──────────────────────────────────────────────────────────
What the CPU sees after MMU translation. Two halves:
Lower: 0x0000000000000000 - 0x0000FFFFFFFFFFFF (user)
Upper: 0xFFFF000000000000 - 0xFFFFFFFFFFFFFFFF (kernel)
┌──────────────────────────────────────────────────────┐
│  0xffff000000000000: vmalloc area                    │
│  0xffff800000000000: linear map (all RAM mapped here)│
│    0xffff800040200000: _text VA (if no KASLR offset) │
│  0xffffe00000000000: vmemmap (struct page array)     │
└──────────────────────────────────────────────────────┘

Layer 3: kimage_voffset — The Bridge
──────────────────────────────────────────────────────────
kimage_voffset = Layer2 - Layer1 = constant for linear map
kimage_voffset = 0xffff800040200000 - 0x40200000
               = 0xffff800000000000 (= PAGE_OFFSET in this example)
```

---

## Why `kimage_voffset` Is NOT Simply `PAGE_OFFSET`

Without KASLR: `kimage_voffset == PAGE_OFFSET` is true.

With KASLR:
- `PAGE_OFFSET` = compile-time constant = start of kernel linear map
- KASLR may shift both PA and VA of the kernel
- `kimage_voffset = PA_shift + VA_shift ≠ PAGE_OFFSET`

Example with KASLR:
```
PAGE_OFFSET = 0xffff800000000000 (compile-time constant)
Kernel loaded at PA 0x78000000 (shifted from default 0x40200000)
Page tables map _text to VA 0xffff800010000000 + 0x38000000 (KASLR VA shift)

kimage_voffset = 0xffff800048000000 - 0x78000000
               = 0xffff7fffff800000   (NOT == PAGE_OFFSET)
```

Using `PAGE_OFFSET` as a compile-time substitute for `kimage_voffset` would be
WRONG on KASLR systems. The runtime value is essential.

---

## Why Compute at `__primary_switched` Time?

At `__primary_switched`:
- MMU is ON ✓ → `adrp x4, _text` gives VA
- `x0` still holds PA of `_text` ✓ → `sub x4, x4, x0` gives correct offset
- C runtime not yet active → must be done in assembly before `start_kernel`

If deferred to C code:
- `x0` (PA) would be lost (overwritten by function calls)
- We'd need another way to get the PA of `_text` (page table walk — expensive)
- C code that calls `__pa()` could run BEFORE `kimage_voffset` is set → wrong results

The assembly-level setup is mandatory.

---

## Relationship to `memstart_addr`

`memstart_addr` is the physical address of the first page of RAM (the base of DRAM).
`kimage_voffset` is the VA-PA gap for the KERNEL IMAGE specifically.

These are related but not the same:
```c
// arch/arm64/mm/init.c
phys_addr_t memstart_addr __ro_after_init;

// kimage_voffset accounts for where the kernel image was placed within RAM
// memstart_addr accounts for where RAM starts in the PA space
```

`kimage_voffset` is used for kernel symbol address conversions.
`memstart_addr` is used for `struct page` array indexing and memory zone calculations.
Both are computed at boot and marked `__ro_after_init`.

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