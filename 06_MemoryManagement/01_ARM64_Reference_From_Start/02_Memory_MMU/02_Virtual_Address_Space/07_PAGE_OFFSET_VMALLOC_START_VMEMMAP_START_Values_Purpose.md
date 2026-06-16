# PAGE_OFFSET, VMALLOC_START, VMEMMAP_START: Values and Purpose

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

Linux ARM64 defines a set of compile-time and boot-time constants that mark the boundaries of each kernel VA region. These constants are used pervasively throughout the kernel for:
- Determining if a VA is in the linear map vs vmalloc vs vmemmap
- Converting between physical and virtual addresses
- Checking pointer validity
- Computing page table indices

Understanding these constants precisely is essential for reading kernel crash dumps, understanding OOM behavior, and debugging memory corruption.

---

## 2. PAGE_OFFSET — Linear Map Base

```c
// arch/arm64/include/asm/memory.h

// For 48-bit VA:
#define PAGE_OFFSET    UL(0xffff000000000000)

// For 39-bit VA:
#define PAGE_OFFSET    UL(0xffffff8000000000)

// General formula:
// PAGE_OFFSET = -(1UL << (VA_BITS - 1))
// = 0 - 2^47 = 0xFFFF_8000_0000_0000 for 48-bit... 
// Actually: PAGE_OFFSET = UINT64_MAX - (1 << VA_BITS) + 1
// = 2^64 - 2^48 = 0xFFFF_0000_0000_0000 for 48-bit
```

**Meaning**: All physical RAM starts being mapped at VA = `PAGE_OFFSET`. The kernel's linear map spans from `PAGE_OFFSET` upward.

```
PA = 0x80000000 (2 GB — typical ARM64 DRAM start)
VA = PA + PAGE_OFFSET - PHYS_OFFSET
   = 0x80000000 + 0xFFFF000000000000 - 0x80000000
   = 0xFFFF000000000000
   (if PHYS_OFFSET = 0x80000000)
```

### Why Physical Offset Matters

On ARM64 systems, RAM doesn't always start at PA=0. `PHYS_OFFSET` records the actual start of RAM:

```c
// arch/arm64/include/asm/memory.h
extern s64 memstart_addr;
#define PHYS_OFFSET     ({ VM_BUG_ON(memstart_addr & 1); memstart_addr; })

// Conversion macros:
#define __pa_symbol(x)  __phys_addr_symbol(RELOC_HIDE((unsigned long)(x), 0))
#define __va(x)         ((void *)(__phys_to_virt((phys_addr_t)(x))))
#define __pa(x)         __virt_to_phys((unsigned long)(x))
```

---

## 3. VMALLOC_START and VMALLOC_END

```c
// arch/arm64/include/asm/pgtable.h

// VMALLOC region is between modules and the linear map,
// leaving room for KASAN shadow and guard pages.

#define VMALLOC_START   (MODULES_VADDR - SZ_256M)
// SZ_256M = 256 MB guard gap between vmalloc and modules

#define VMALLOC_END     (- PUD_SIZE - VMEMMAP_SIZE - SZ_64K)
// PUD_SIZE = 512 GB (for 4KB, 48-bit VA)
// This positions VMALLOC_END well below vmemmap

// Typical values for 48-bit VA, 4KB pages:
// VMALLOC_START ≈ 0xFFFF800000000000  (varies with KASLR)
// VMALLOC_END   ≈ 0xFFFFC00000000000  (varies with vmemmap size)
```

### vmalloc Size

```
vmalloc size = VMALLOC_END - VMALLOC_START
For 48-bit VA: approximately 128 TB
This is enormous — far more than any current system uses.
```

### What Lives in vmalloc

1. `vmalloc()` allocations
2. `ioremap()` device mappings (MMIO)
3. `vmap()` explicit VA mappings
4. Per-CPU base allocation
5. `__get_vm_area()` and friends
6. Stacks (VMAP_STACK): `CONFIG_VMAP_STACK=y` maps task kernel stacks in vmalloc region

---

## 4. VMEMMAP_START

```c
// arch/arm64/include/asm/memory.h

// vmemmap: a virtual array of struct page, one per PFN
// struct page size = 64 bytes (typically)
// For N physical pages: vmemmap size = N * 64

#define VMEMMAP_START    (-(UL(1) << (VA_BITS - VMEMMAP_SHIFT)))

// VMEMMAP_SHIFT = PAGE_SHIFT - STRUCT_PAGE_MAX_SHIFT
// PAGE_SHIFT = 12 (4KB pages)
// STRUCT_PAGE_MAX_SHIFT = 6 (struct page = 64 bytes = 2^6)
// VMEMMAP_SHIFT = 12 - 6 = 6

// For 48-bit VA:
// VMEMMAP_START = -(1 << (48 - 6)) = -(1 << 42)
//              = 0xFFFF_FC00_0000_0000

// vmemmap pointer in the kernel:
extern struct page *vmemmap;
// Set at boot to: vmemmap = VMEMMAP_START - (memstart_addr >> PAGE_SHIFT)
// This aligns PFN indexing with the physical memory start
```

---

## 5. MODULES_VADDR and MODULES_END

```c
// arch/arm64/include/asm/memory.h

// Modules are loaded near the kernel image for branch range:
// ARM64 BL instruction: ±128 MB range
// Modules must be within ±128 MB of kernel text

#define KIMAGE_VADDR    (MODULES_END)
#define MODULES_END     (KIMAGE_VADDR)   // Circular but set by linker/KASLR
#define MODULES_VADDR   (KIMAGE_VADDR - SZ_128M)

// With KASLR: KIMAGE_VADDR is randomized at boot within the module region
```

---

## 6. FIXADDR_START and Fixed Mappings

```c
// arch/arm64/include/asm/fixmap.h

// Fixed virtual addresses near top of kernel VA space:
#define FIXADDR_SIZE    (__end_of_permanent_fixed_addresses << PAGE_SHIFT)
#define FIXADDR_START   (FIXADDR_TOP - FIXADDR_SIZE)
#define FIXADDR_TOP     (PCI_IO_END)

// Fixed mappings are used for:
// - Early console (before ioremap is ready)
// - EFI runtime services
// - CPU-local early I/O
// - KMAP_ATOMIC (no longer used on modern kernels)
```

---

## 7. Memory Layout Diagram (48-bit VA, 4KB, typical values)

```
VA                              Region                    PA usage
─────────────────────────────────────────────────────────────────────
0xFFFF_FFFF_FFFF_FFFF ┐
                       │  Fixed mappings (fixmap)         (Device/MMIO)
0xFFFF_FFFE_0000_0000 ┘
0xFFFF_FFFD_0000_0000 ┐
                       │  PCI I/O                         (Device I/O)
0xFFFF_FFFC_0000_0000 ┘
0xFFFF_FC00_0000_0000 ┐
                       │  vmemmap                         (Physical RAM pages)
                       │  struct page array               ~4 TB range
0xFFFF_B800_0000_0000 ┘
[guard page]
0xFFFF_A000_0000_0000 ┐
                       │  vmalloc / ioremap               (Non-contiguous PA)
                       │                                  ~128 TB range
0xFFFF_8000_0000_0000 ┘
0xFFFF_8000_0000_0000 ┐
                       │  Modules (128 MB)                (Physical RAM)
                       │  Kernel image (KASLR)            (Physical RAM)
0xFFFF_7000_0000_0000 ┘
0xFFFF_0000_0000_0000 ┐
                       │  Linear map (direct RAM mapping) PA 0x8000_0000...
                       │  All physical RAM here           = PA + PAGE_OFFSET
                       │  256 TB range
0xFFFF_0000_0000_0000 ┘ ← PAGE_OFFSET

[canonical hole — non-mappable]

0x0000_FFFF_FFFF_FFFF ┐
                       │  User space                      (per-process PA)
                       │  stack, heap, mmap, text         (from TTBR0)
                       │  256 TB range
0x0000_0000_0000_0000 ┘
```

---

## 8. Runtime Inspection of VA Layout

Linux provides /proc/iomem and /proc/vmallocinfo, but for kernel constants:

```bash
# View kernel virtual memory layout:
cat /proc/kallsyms | grep -E "PAGE_OFFSET|vmemmap|_stext"

# View vmalloc info:
cat /proc/vmallocinfo | head -20

# View physical memory regions:
cat /proc/iomem

# Kernel log shows VA layout at boot:
dmesg | grep -E "PAGE_OFFSET|VMALLOC|vmemmap|Memory:"
```

Example boot log:
```
[    0.000000] Virtual kernel memory layout:
[    0.000000]     modules : 0xffff800000000000 - 0xffff800008000000   (   128 MB)
[    0.000000]     vmalloc : 0xffff800008000000 - 0xffffbdfdfee00000   (   ~238 TB)
[    0.000000]      .text  : 0xffff800008080000 - 0xffff8000093a3000   ( 19340 KB)
[    0.000000]       .init : 0xffff8000093a3000 - 0xffff800009680000   (  2292 KB)
[    0.000000]       .data : 0xffff800009680000 - 0xffff800009f1f000   (  9212 KB)
[    0.000000]        .bss : 0xffff800009f1f000 - 0xffff80000a2c5000   (  3736 KB)
[    0.000000]     fixed   : 0xffffbdfdfee00000 - 0xffffbdfdff000000   (  2048 KB)
[    0.000000]     PCI I/O : 0xffffbdfdff000000 - 0xffffbdfe00000000   (    16 MB)
[    0.000000]     vmemmap : 0xffffbdfe00000000 - 0xfffffdfe00000000   ( 65536 GB max)
[    0.000000]     memory  : 0xfffffc0000000000 - 0xffffff0000000000   (   ~48 TB)
```

---

## 9. Interview Questions & Answers

**Q1: What is PAGE_OFFSET and how do you use it to convert PA to VA?**

`PAGE_OFFSET` is the base virtual address of the kernel linear map. All physical RAM is mapped starting at this address. The conversion formula is `VA = PA - PHYS_OFFSET + PAGE_OFFSET` (where `PHYS_OFFSET` = `memstart_addr` = the physical address where RAM starts). This macro is `phys_to_virt(pa)`. The reverse is `virt_to_phys(va) = va - PAGE_OFFSET + PHYS_OFFSET`. These are O(1) operations — just arithmetic. They only work for linear map addresses; vmalloc addresses require `vmalloc_to_page()` followed by `page_to_phys()`.

**Q2: Why is vmemmap important for kernel performance?**

vmemmap enables O(1) conversion between a Page Frame Number (PFN) and its `struct page` via simple pointer arithmetic: `page = vmemmap + pfn`. Without vmemmap (e.g., flat memory model on smaller systems), a flat array suffices. With sparse memory (holes in the physical address space — typical on NUMA or large servers), vmemmap only allocates backing memory for the sections of `struct page` array that correspond to present physical memory sections. This avoids wasting memory on holes in the PA space while preserving the O(1) conversion property.

**Q3: Why are kernel modules loaded near KIMAGE_VADDR instead of in the linear map?**

ARM64 direct branch instructions (BL, B) use a 26-bit signed immediate, giving a branch range of ±128 MB. If a module is more than 128 MB away from the kernel text, a direct `BL kernel_function` from the module would be out of range and require a veneer (PLT stub). By loading modules in the 128 MB window just below KIMAGE_VADDR, direct branches from modules to kernel code (and vice versa) work without veneers. This reduces module size and improves call performance.

---

## 10. Quick Reference

| Constant | 48-bit Value | Purpose |
|---|---|---|
| `PAGE_OFFSET` | `0xFFFF_0000_0000_0000` | Linear map base |
| `PHYS_OFFSET` (`memstart_addr`) | Board-specific (e.g., `0x8000_0000`) | Physical RAM start |
| `VMALLOC_START` | ~`0xFFFF_8000_0000_0000` | vmalloc/ioremap base |
| `VMALLOC_END` | ~`0xFFFF_C000_0000_0000` | vmalloc top |
| `VMEMMAP_START` | ~`0xFFFF_FC00_0000_0000` | struct page array base |
| `MODULES_VADDR` | ~`0xFFFF_8000_0000_0000` | Module load VA start |
| `KIMAGE_VADDR` | ~`0xFFFF_8000_0800_0000` | Kernel image VA (KASLR) |
| `FIXADDR_START` | ~`0xFFFF_FFFE_0000_0000` | Fixed VA mappings |
