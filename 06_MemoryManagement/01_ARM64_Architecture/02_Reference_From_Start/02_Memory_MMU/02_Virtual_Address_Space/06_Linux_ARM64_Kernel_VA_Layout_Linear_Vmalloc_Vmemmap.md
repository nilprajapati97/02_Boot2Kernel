# Linux ARM64 Kernel VA Layout: Linear Map, Vmalloc, Vmemmap

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

The Linux kernel on ARM64 divides the TTBR1 VA region (kernel space) into several distinct regions, each serving a specific purpose:

1. **Linear map** (also called the direct map): Direct 1:1 mapping of all physical RAM into kernel VA space. `PA → VA = PA + PAGE_OFFSET`. Used for most kernel data, kmalloc, slab, etc.

2. **vmalloc region**: Non-contiguous virtual memory for kernel allocations that don't need physically contiguous memory. Used by `vmalloc()`, `ioremap()`, module loading.

3. **vmemmap**: The `struct page` array mapped here. Each physical page frame has a `struct page` entry. `vmemmap` maps these efficiently for sparse memory systems.

4. **Kernel image**: The kernel binary itself (text, data, BSS) loaded at a KASLR-randomized address.

5. **Modules region**: Loadable kernel modules loaded near the kernel image (within ±128 MB for branch reach).

6. **Fixed mappings / EFI**: Special fixed VA mappings for early boot, EFI runtime services.

---

## 2. ARM64 Kernel VA Layout (48-bit VA, 4KB Pages)

```
Virtual Address Range          | Region                    | Size
───────────────────────────────┼───────────────────────────┼──────────────
0xFFFF_FFFF_FFFF_FFFF          |                           |
  ↓                            | Fixed mappings (fixmap)   | 4 MB
0xFFFF_FFFE_0000_0000          |                           |
  ↓                            | PCI I/O space             | 16 MB
0xFFFF_FFFD_0000_0000          |                           |
  ↓                            | [guard]                   |
0xFFFF_C000_0000_0000 (approx) | vmemmap (struct page arr) | ~4 TB max
  ↓                            |                           |
0xFFFF_B000_0000_0000 (approx) | [guard / sparse]          |
  ↓                            |                           |
0xFFFF_A000_0000_0000 (approx) | vmalloc / ioremap         | ~128 TB
  ↓                            |                           |
0xFFFF_8000_0000_0000 (approx) | Modules                   | 128 MB
  ↓                            |                           |
0xFFFF_8000_0000_0000          | Kernel image (KIMAGE)     | ~128 MB
  (KASLR-randomized offset)    |                           |
  ↓                            |                           |
0xFFFF_0000_0000_0000          | Linear map (PAGE_OFFSET)  | up to 256 TB
  ↓  Physical RAM mapped here  |                           |
0xFFFF_0000_0000_0000          | = PA 0x0 mapped here      |
```

**Note**: Exact boundaries vary by kernel configuration and KASLR randomization. The above shows the general scheme.

---

## 3. Precise Values from Linux Source

```c
// arch/arm64/include/asm/memory.h (simplified, 48-bit VA):

// Linear map start:
#define PAGE_OFFSET     UL(0xffff000000000000)

// vmemmap:
#define VMEMMAP_START   (-(UL(1) << (VA_BITS - VMEMMAP_SHIFT)))
// For 48-bit VA with 4KB pages:
// VMEMMAP_SHIFT = 6 (each struct page = 64 bytes, so 2^6)
// VMEMMAP_START = -(1 << (48-6)) = -(1 << 42) = 0xFFFF_FC00_0000_0000

// vmalloc:
#define VMALLOC_START   (MODULES_VADDR - SZ_256M)
#define VMALLOC_END     (PAGE_OFFSET - PUD_SIZE - VMEMMAP_SIZE - SZ_64K)

// Modules:
#define MODULES_VADDR   (KIMAGE_VADDR - KASAN_SHADOW_SIZE)
#define MODULES_END     (KIMAGE_VADDR)

// Kernel image:
#define KIMAGE_VADDR    (MODULES_END)   // = kernel load VA (KASLR randomized)
```

---

## 4. Linear Map (Direct Map)

### What It Is

The linear map is the most important kernel VA region. It provides a **direct** virtual-to-physical mapping for all physical RAM:

```
PA → VA:  VA = PA + PAGE_OFFSET
VA → PA:  PA = VA - PAGE_OFFSET

Example:
  PA = 0x4000_0000 (1 GB offset, typical for ARM64 boards)
  VA = 0x4000_0000 + 0xFFFF_0000_0000_0000
     = 0xFFFF_0000_4000_0000
```

### Why It Exists

The kernel needs to access arbitrary physical memory quickly. Without the linear map, every kernel buffer would need an explicit `ioremap()` or `kmap()`. With the linear map, the kernel can dereference any physical address just by adding PAGE_OFFSET — a single addition, no page table walk needed (TLB already has the kernel entries).

### Page Size and Entries

For a 4KB page, 48-bit VA, the linear map uses PUD-level block mappings (1 GB blocks) where possible:

```
Each 1 GB of RAM → one PMD/PUD entry in the linear map
(block descriptor, 1 GB block)
This minimizes page table memory and improves TLB coverage.

For 2 MB aligned regions: L2 block descriptors
For 4 KB residual regions: L3 page descriptors
```

```c
// arch/arm64/mm/mmu.c
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_stext);
    phys_addr_t kernel_end   = __pa_symbol(__init_end);
    
    // Map all memblock-reserved regions:
    for_each_mem_range(i, &start, &end) {
        if (start >= end) continue;
        // Create linear map entries for PA [start, end)
        __map_memblock(pgdp, start, end, pgprot_tagged(PAGE_KERNEL), flags);
    }
}
```

---

## 5. vmalloc Region

`vmalloc()` allocates **virtually contiguous** but **physically non-contiguous** memory. Used when:
- Large allocations are needed but physical contiguity can't be guaranteed
- Kernel modules (loaded at non-contiguous PAs)
- `ioremap()` for device registers (device PA → kernel VA mapping)

```c
// Allocation example:
void *p = vmalloc(size);
// p points to a VA in the vmalloc region
// Physical pages are individually allocated (not necessarily contiguous)
// The vmalloc page tables (in TTBR1) map each VA page to a separate PA

struct vm_struct {
    struct vm_struct    *next;
    void                *addr;    // VM_AREA start VA
    unsigned long       size;     // size + PAGE_SIZE (guard)
    unsigned long       flags;
    struct page         **pages;  // array of struct page * per sub-page
    unsigned int        nr_pages;
    phys_addr_t         phys_addr; // for ioremap only
    const void          *caller;  // allocation site
};
```

### ioremap vs vmalloc

```
vmalloc():   Allocates physical pages, maps to vmalloc VA
             Physical pages come from page allocator
             Used for kernel data that's large but not DMA

ioremap():   Maps device MMIO PA range to vmalloc VA
             Does NOT allocate physical pages
             Physical address is a device register region (not RAM)
             Page attributes: Device-nGnRnE (strongly ordered, non-cacheable)
```

---

## 6. vmemmap — The struct page Array

ARM64 uses **sparse memory** model. For each physical page frame (PFN), there is a `struct page` in the kernel. vmemmap provides a compact, indexed array for these:

```
struct page size: typically 64 bytes (on 4KB page, 64B struct)

For N physical pages:
  vmemmap region = N × 64 bytes

Sparse memory splits this into sections:
  Each SECTION (typically 128 MB of RAM) has its own vmemmap segment
  Only present sections have vmemmap entries allocated

Fast PFN → struct page conversion:
  struct page *p = vmemmap + pfn;
  // vmemmap is the base address of the struct page array
  // Direct indexed access — no hash, no list walk

Reverse: struct page * → PFN:
  unsigned long pfn = p - vmemmap;
```

```c
// arch/arm64/include/asm/memory.h
#define vmemmap     ((struct page *)VMEMMAP_START - (memstart_addr >> PAGE_SHIFT))

// Usage:
#define __pfn_to_page(pfn)   (vmemmap + (pfn))
#define __page_to_pfn(page)  ((unsigned long)((page) - vmemmap))

// These are O(1) operations — just pointer arithmetic!
```

---

## 7. KASAN Shadow Memory

When `CONFIG_KASAN=y`, an additional shadow memory region is mapped:

```
Shadow memory: 1/8 of total address space (each byte covers 8 bytes of real memory)
Shadow base: depends on KASAN variant

KASAN uses a portion of the vmalloc region or a dedicated shadow region:
For generic KASAN:
  shadow_offset = KASAN_SHADOW_OFFSET
  shadow_addr = (addr >> 3) + KASAN_SHADOW_OFFSET

For tag-based KASAN (arm64 hardware):
  Uses MTE hardware instead of shadow memory
```

---

## 8. Physical vs Virtual Conversion Functions

```c
// arch/arm64/include/asm/memory.h
static inline phys_addr_t __virt_to_phys_nodebug(unsigned long x)
{
    phys_addr_t y = (__u64)x;
    // If in linear map:
    if (likely(y >= PAGE_OFFSET))
        return y - PAGE_OFFSET + PHYS_OFFSET;
    // If in vmalloc: must use page table walk or vmalloc_to_page()
    BUG();
}

#define virt_to_phys(x)    __virt_to_phys((unsigned long)(x))
#define phys_to_virt(x)    ((void *)__phys_to_virt((phys_addr_t)(x)))

// PA → VA (linear map):
#define __phys_to_virt(x)  ((unsigned long)((x) - PHYS_OFFSET + PAGE_OFFSET))

// VA → PA (linear map):
#define __virt_to_phys(x)  ((phys_addr_t)((x) - PAGE_OFFSET + PHYS_OFFSET))
```

---

## 9. Interview Questions & Answers

**Q1: What is the linear map and why is it important?**

The linear map is a kernel VA region where all physical RAM is mapped directly and contiguously. The mapping formula is `VA = PA + PAGE_OFFSET`. This gives the kernel O(1) access to any physical address — just add a constant offset. It's critical for efficient kernel operations: `kmalloc()` returns linear map addresses, so the kernel can immediately use physical page contents without any TLB miss for the VA→PA translation (the mapping is always present and global). Without the linear map, every kernel memory access would require either a page table walk or an explicit mapping call.

**Q2: What is vmemmap and how does it enable O(1) PFN-to-page conversion?**

vmemmap is a VA region where an array of `struct page` structures is mapped, indexed by Page Frame Number (PFN). The base address `vmemmap` is chosen so that `vmemmap + pfn` gives the `struct page` for that PFN. Converting a PFN to its `struct page` is just a single pointer addition. The reverse (page to PFN) is also O(1): `pfn = page - vmemmap`. For sparse memory systems (where not all PFNs are present), only the vmemmap sections corresponding to present memory are actually backed by physical pages.

**Q3: Why is vmalloc used for kernel modules instead of kmalloc?**

Kernel modules can be hundreds of KB to MB in size. `kmalloc()` requires physically contiguous memory, which becomes scarce for large allocations (memory fragmentation). `vmalloc()` only requires virtually contiguous memory — the underlying physical pages can be scattered throughout RAM. Additionally, modules need executable memory, so their vmalloc mappings use appropriate page attributes (executable, not writeable after init). Modules are loaded in the modules region (near the kernel image) to ensure that direct call instructions (`BL`) can reach the kernel — the ±128MB branch range requires proximity.

---

## 10. Quick Reference

| Region | VA Start (48-bit) | Purpose |
|---|---|---|
| Linear map | 0xFFFF_0000_0000_0000 | All RAM, direct PA→VA |
| vmalloc | 0xFFFF_8000_0000_0000 (approx) | vmalloc, ioremap |
| Modules | 0xFFFF_8000_0000_0000 (near kernel) | Loadable modules |
| Kernel image | KIMAGE_VADDR (KASLR) | Kernel text/data |
| vmemmap | 0xFFFF_FC00_0000_0000 (approx) | struct page array |
| Fixed map | Near 0xFFFF_FFFF_FFFF_FFFF | fixmap, EFI |

| Function | Direction | Region |
|---|---|---|
| `phys_to_virt()` | PA → VA | Linear map |
| `virt_to_phys()` | VA → PA | Linear map |
| `vmalloc_to_page()` | VA → struct page | vmalloc |
| `pfn_to_page(pfn)` | PFN → struct page | vmemmap |
| `page_to_pfn(page)` | struct page → PFN | vmemmap |
