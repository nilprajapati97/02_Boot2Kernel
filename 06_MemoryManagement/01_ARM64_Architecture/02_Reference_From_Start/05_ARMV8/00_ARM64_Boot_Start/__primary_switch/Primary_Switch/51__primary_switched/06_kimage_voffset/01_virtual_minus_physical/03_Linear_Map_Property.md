# Linear Map — Constant Offset Property and Its Limits

## What "Linear Map" Means

A LINEAR MAP (or "flat map" or "direct map") is a page table mapping where
the VA-PA relationship is a constant offset:

```
VA = PA + CONSTANT   (for all PA in some range)
```

On ARM64 Linux:
```
Linear map: VA = PA + PAGE_OFFSET  (approximately)
More precisely: VA = PA + kimage_voffset
```

This allows `__pa(va)` to be a SINGLE SUBTRACTION rather than a page table walk.

---

## How the Linear Map Is Implemented in ARM64 Page Tables

The linear map uses BLOCK entries (not page entries) for efficiency:

**4-level paging (4 KB pages, 48-bit VA):**
```
Level 0 (PGD): 512 GB per entry → 1 entry for all of 0xffff800000000000 linear map
Level 1 (PUD): 1 GB per entry   → each entry = 1 GB block
Level 2 (PMD): 2 MB per entry   → each entry = 2 MB block
Level 3 (PTE): 4 KB per entry   → individual pages
```

For efficiency, the linear map uses 2 MB blocks (PMD-level):
- One Level 2 entry = 2 MB of physically contiguous RAM
- PA 0x40000000 → VA 0xffff800040000000: one PMD block entry
- No Level 3 PTE table needed for this 2 MB range
- TLB uses one entry to cover 2 MB (instead of 512 PTEs for 4 KB pages)

---

## The Linear Map Does NOT Cover All Virtual Addresses

`kimage_voffset` only applies to addresses in the linear map range:
```
PA range: 0 to DRAM_SIZE (max ~512 GB on 48-bit VA systems)
VA range: PAGE_OFFSET to PAGE_OFFSET + DRAM_SIZE

Specifically for kernel code/data:
    VA range: 0xffff800040000000 (example) to 0xffff800080000000 (2 GB kernel image max)
    kimage_voffset applies here ✓

Does NOT apply to:
    vmalloc area:   0xffff000000000000 - 0xffff800000000000
    vmemmap area:   0xfffffe0000000000 - 0xfffffe8000000000
    fixmap area:    0xfffffbffc0000000 - 0xfffffbffe0000000
    module area:    near _etext (vmalloc-based)
```

Calling `__pa()` on any address outside the linear map gives wrong results.
`CONFIG_DEBUG_VIRTUAL` adds bounds checking to catch this.

---

## When the Linear Map Breaks Down — HUGE_VMAP

`CONFIG_HUGE_VMAP` allows vmalloc to use 2 MB pages. The vmalloc area does NOT
use `kimage_voffset` for conversion — it uses a different mechanism.

`CONFIG_VMAP_STACK` (kernel stack via vmalloc) also uses a different PA conversion:
```c
// Getting PA of a vmalloc-stack page:
struct page *page = vmalloc_to_page(stack_va);
phys_addr_t pa = page_to_phys(page);  // uses pfn_to_phys(), not kimage_voffset
```

The linear map assumption `PA = VA - kimage_voffset` is only valid for:
1. Kernel text/data/BSS (mapped in linear map)
2. `kmalloc`/`kzalloc` allocations (from slab, in linear map)
3. Direct memory accesses to known physical ranges

---

## Hole in the Linear Map — Memory-Mapped I/O

Physical memory ranges used for MMIO (Memory-Mapped I/O) are NOT typically
included in the linear map:
```
PA 0x00000000 - 0x3FFFFFFF: Possible MMIO (no RAM) — NOT in linear map
PA 0x40000000 - 0x3FFFFFFFF: RAM — IN linear map

kimage_voffset does NOT work for MMIO PAs because:
- VA = PA + kimage_voffset → wrong VA (would be in linear map range, but no mapping there)
- MMIO must be accessed via ioremap() which creates a separate vmalloc mapping
```

`ioremap()` maps MMIO PA to a vmalloc VA. The resulting VA is NOT `PA + kimage_voffset`.
It's a separate, explicitly-managed mapping.

---

## Numerical Verification with Real Kernel

```bash
# On a running ARM64 system:

# Get _text PA from /proc/iomem:
$ grep "Kernel code" /proc/iomem
  40200000-41ffffff : Kernel code

# Get _text VA from /proc/kallsyms:
$ grep " T _text$" /proc/kallsyms
  ffff800010200000 T _text  # (address may be offset by KASLR)

# Compute kimage_voffset:
# VA = 0xffff800010200000, PA = 0x40200000
# kimage_voffset = 0xffff800010200000 - 0x40200000 = 0xffff7fffd0000000

# Verify using another symbol:
$ grep " D kimage_voffset" /proc/kallsyms
  ffff800012345678 D kimage_voffset

# kimage_voffset variable PA = 0xffff800012345678 - 0xffff7fffd0000000
#                           = 0x42345678
# Should match /proc/iomem in the data range
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