# ARM64 Virtual Memory Layout — Full Map

## The 48-bit VA Space (4-level Page Tables, Default)

ARM64 supports multiple VA widths. Linux 5.x defaults to 48-bit VAs with 4 KB pages.
The address space is split into two halves by the hardware:
- Bits [63:48] = all zeros → user space (EL0 accesses via TTBR0_EL1)
- Bits [63:48] = all ones → kernel space (EL1 accesses via TTBR1_EL1)

```
64-bit Virtual Address Space:
┌─────────────────────────────────────────────────────────────────────────────┐
│  0x0000000000000000 ─────────── User Space (TTBR0) ──────────────────────  │
│                                                                             │
│  0x0000000000000000: Text, data of process                                  │
│  0x0000800000000000: Top of user space (48-bit)                             │
│                                                                             │
│  ════════════════════ NON-CANONICAL GAP ════════════════════                │
│                                                                             │
│  0xffff000000000000: ──────── Kernel Space (TTBR1) ──────────────────────  │
│                                                                             │
│  0xffff000000000000 ┤ vmalloc area start                                    │
│                     │ (dynamically allocated virtual ranges)                │
│                     │ includes kernel modules                               │
│  0xffff7fffffffffff ┤ vmalloc area end                                      │
│                                                                             │
│  0xffff800000000000 ┤ PAGE_OFFSET = linear map start                        │
│                     │ ALL physical RAM is linearly mapped here              │
│                     │ PA 0x0 → VA 0xffff800000000000                        │
│                     │ PA 0x40200000 → VA 0xffff800040200000                 │
│                     │   ↑ kernel _text maps here (no KASLR)                 │
│  0xfffffdffffffffff ┤ linear map end (512 GB of RAM supported)              │
│                                                                             │
│  0xfffffe0000000000 ┤ vmemmap: struct page array                            │
│                     │ Each struct page is 64 bytes                          │
│                     │ PA 0x40200000 → vmemmap[PA>>12]                       │
│  0xfffffefbffffffff ┤ vmemmap end                                           │
│                                                                             │
│  0xffffff8000000000 ┤ PCI I/O space (if used)                               │
│                                                                             │
│  0xffffffffffffffff ┤ top of 64-bit address space                           │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## How KASLR Shifts the Layout

With KASLR, the kernel image within the linear map is shifted by a random amount:

```
Without KASLR:
  _text PA = 0x40200000
  _text VA = PAGE_OFFSET + 0x40200000 = 0xffff800040200000
  kimage_voffset = PAGE_OFFSET = 0xffff800000000000

With KASLR (VA randomization):
  _text PA = 0x40200000 (may also change with PA randomization)
  _text VA = PAGE_OFFSET + 0x40200000 + kaslr_va_shift
           = 0xffff800040200000 + 0x28000000  (example 640 MB shift)
           = 0xffff800068200000
  kimage_voffset = 0xffff800068200000 - 0x40200000
                 = 0xffff800028000000  (NOT == PAGE_OFFSET)
```

The VA randomization is applied by choosing a different entry point in the page
tables for the kernel image mapping.

---

## The Linear Map — Why It Exists

The "linear map" (also called "direct map" or "physmap") is a 1:1 mapping of all
physical RAM into kernel virtual addresses. Benefits:

1. **Simplicity**: any PA → corresponding VA = PA + PAGE_OFFSET + optional_shift
   (before KASLR: just PAGE_OFFSET). `__va(pa)` is a single add.

2. **Performance**: kernel code can access any physical memory directly via its
   VA without needing per-access page table manipulation.

3. **No fragmentation**: vmalloc area is kept for dynamic allocations; static
   kernel data uses the linear map with large pages (2 MB or 1 GB) for
   efficiency (fewer TLB entries).

The linear map is set up by `map_kernel_segment` and `__map_initrd_memblock` in
`arch/arm64/mm/mmu.c`. `kimage_voffset` is how the kernel tracks where within
the linear map the image is located.

---

## Page Size Options and Impact on `kimage_voffset`

ARM64 supports three page sizes: 4 KB, 16 KB, 64 KB.

| Page Size | VA width | PAGE_OFFSET | Linear Map Size |
|---|---|---|---|
| 4 KB | 48-bit | 0xffff800000000000 | 512 GB |
| 16 KB | 47-bit | 0xffff800000000000 | 512 GB |
| 64 KB | 42-bit | 0xffff800000000000 | 4 TB |

`kimage_voffset` is computed the same way regardless of page size:
`VA(_text) - PA(_text)`. The value changes with page size because `PAGE_OFFSET`
changes, but the mechanism is identical.

---

## 52-bit VA (ARMv8.2+) Impact

ARMv8.2 systems with `CONFIG_ARM64_VA_BITS=52` extend the VA space:
- Linear map start: `0xff60000000000000` (instead of `0xffff800000000000`)
- More address space for linear map (16 PB instead of 512 GB)
- Supports systems with > 512 GB of RAM

`kimage_voffset` arithmetic still works: `adrp` in `__primary_switched` produces
a 52-bit VA (since PC is a 52-bit VA), and `x0` (PA) is always small, so the
subtraction gives the correct 52-bit `kimage_voffset`.

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