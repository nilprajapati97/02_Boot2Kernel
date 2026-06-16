# pfn_to_page, virt_to_page, and vmemmap on ARM64

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
The kernel needs to convert between three representations of a physical page:
  1. Physical Address (PA): actual bus address (0x40000000, etc.)
  2. Page Frame Number (PFN): PA >> 12 (index in physical page array)
  3. struct page *: kernel's metadata descriptor for that page
  4. Kernel Virtual Address (KVA): PA + PAGE_OFFSET (linear mapped)

Why all these representations?
  PA:          Used by hardware (DMA, page tables, TTBR0/1)
  PFN:         Index into memory model (vmemmap array)
  struct page: Contains all metadata (flags, refcount, mapping, etc.)
  KVA:         How the CPU accesses memory through the linear map

Conversions must be:
  O(1): called millions of times per second during page fault handling
  Memory-efficient: can't afford large lookup tables
  
ARM64 solution: vmemmap (virtual memory map)
  vmemmap: a contiguous virtual address range mapped to struct page array
  Physical layout: struct page pages may be scattered in NUMA memory
  Virtual layout: vmemmap[pfn] is ALWAYS at vmemmap_base + pfn * sizeof(struct page)
  
  This makes pfn_to_page: O(1) pointer arithmetic
  And page_to_pfn: O(1) pointer arithmetic
```

---

## 2. vmemmap Architecture on ARM64

```
ARM64 virtual memory layout (48-bit VA, TTBR1):

  0xFFFF_0000_0000_0000: kernel VA start (PAGE_OFFSET)
  +-----------------------------------+
  |                                   |
  |  Linear mapping (direct map)      |  ← __pa()/__va() work here
  |  PA 0 → KVA PAGE_OFFSET + 0       |
  |  PA X → KVA PAGE_OFFSET + X       |
  |                                   |
  +-----------------------------------+
  0xFFFF_8000_0000_0000: vmalloc area start (VMALLOC_START)
  +-----------------------------------+
  |  vmalloc / ioremap space          |
  |                                   |
  +-----------------------------------+
  0xFFFF_E000_0000_0000: vmemmap (VMEMMAP_START)
  +-----------------------------------+
  |  struct page array                |
  |  vmemmap[0] = page for PFN 0      |
  |  vmemmap[1] = page for PFN 1      |
  |  vmemmap[N] = page for PFN N      |
  |  Each entry = 64 bytes            |
  |  Size = max_pfn × 64 bytes        |
  +-----------------------------------+
  0xFFFF_FFFF_8000_0000: kernel image (kimage_vaddr)
  +-----------------------------------+
  |  .text, .rodata, .data, .bss      |
  +-----------------------------------+

vmemmap address:
  ARM64 (arch/arm64/include/asm/memory.h):
  #define VMEMMAP_START  (-(UL(1) << (VA_BITS - 1)) >> 1)  // tuned to VA size
  
  For 48-bit VA (VA_BITS=48):
    VMEMMAP_START = 0xFFFF_FFF8_0000_0000 (rough approximation)
  
  Each struct page = 64 bytes
  For 256GB RAM: max_pfn = 256GB / 4KB = 64M pages
    vmemmap size = 64M × 64B = 4GB of VA used for vmemmap
    
  The vmemmap VA range is backed by REAL physical pages
  (struct page metadata itself needs physical RAM)
  Overhead: 64 bytes per 4KB page = 1.5% of RAM for struct page array
```

---

## 3. pfn_to_page and page_to_pfn

```c
/* arch/arm64/include/asm/mem_map.h or generic include/asm-generic/memory_model.h */

/* vmemmap-based (ARM64 uses this): */

extern struct page *vmemmap;  // points to VMEMMAP_START (virtual)

#define pfn_to_page(pfn)    (vmemmap + (pfn))
#define page_to_pfn(page)   ((unsigned long)((page) - vmemmap))

/* Example:
   pfn = 0x40000 (PA = 0x40000000, 1GB offset)
   vmemmap = 0xFFFF_FFF8_0000_0000 (hypothetical)
   pfn_to_page(0x40000) = 0xFFFF_FFF8_0000_0000 + 0x40000 × 64
                        = 0xFFFF_FFF8_0000_0000 + 0x0100_0000
                        = 0xFFFF_FFF8_0100_0000
*/

/* Assembly view of pfn_to_page on ARM64:
   x0 = pfn (input)
   adrp x1, vmemmap                  // load vmemmap base
   ldr  x1, [x1, :lo12:vmemmap]
   add  x0, x1, x0, lsl #6          // x0 = vmemmap + pfn * 64
                                     // lsl #6 = multiply by 64 (sizeof struct page)
   ret
*/

/* virt_to_page: kernel linear VA → struct page */
#define virt_to_page(kaddr)    pfn_to_page(virt_to_pfn(kaddr))
/* where virt_to_pfn(kaddr) = __pa(kaddr) >> PAGE_SHIFT */
/* = ((unsigned long)(kaddr) - PAGE_OFFSET) >> 12 */

/* page_to_virt: struct page → kernel linear VA */
#define page_to_virt(page)    __va(page_to_phys(page))
/* page_to_phys(page) = page_to_pfn(page) << PAGE_SHIFT */
/* __va(pa) = (pa) + PAGE_OFFSET */
```

---

## 4. phys_to_virt and virt_to_phys

```c
/* arch/arm64/include/asm/memory.h */

/* PAGE_OFFSET: start of kernel linear mapping in virtual address space */
#define PAGE_OFFSET   (UL(0xFFFF000000000000))  // ARM64 48-bit
/* Actual value computed from VA_BITS and CONFIG */

/* __pa(): kernel virtual address → physical address */
#define __pa(x)   ((unsigned long)(x) - PAGE_OFFSET + PHYS_OFFSET)
/* PHYS_OFFSET: physical start address of RAM (often 0 on ARM64, but may differ) */
/* If PHYS_OFFSET = 0:  __pa(kva) = kva - PAGE_OFFSET */

/* __va(): physical address → kernel virtual address */
#define __va(x)   ((void *)((unsigned long)(x) - PHYS_OFFSET + PAGE_OFFSET))

/* Helpers (same thing, nicer naming): */
static inline phys_addr_t virt_to_phys(const volatile void *x)
{
    return __pa(x);
}

static inline void *phys_to_virt(phys_addr_t x)
{
    return __va(x);
}

/* IMPORTANT LIMITATION:
   These ONLY work for addresses in the LINEAR MAPPING (direct map)!
   
   Valid:
     char buf[128];             // stack (kernel thread, linear mapped)
     struct page *p = ...;      // vmemmap (NOT linear mapped! different region)
     unsigned long *kdata = kmalloc(64, GFP_KERNEL); // slab (linear mapped)
     
   INVALID (WRONG RESULTS):
     void *vmalloc_ptr = vmalloc(1024);
     phys_addr_t pa = virt_to_phys(vmalloc_ptr);  // WRONG! not linear mapped
     
     void *ioremap_ptr = ioremap(mmio_base, 0x1000);
     phys_addr_t pa = virt_to_phys(ioremap_ptr);  // WRONG! not linear mapped
     
   For vmalloc: use vmalloc_to_pfn(va) or vmalloc_to_page(va)
   For ioremap: use __io_virt_to_phys() or keep track of PA manually
*/
```

---

## 5. struct page Memory Overhead Analysis

```
struct page size: sizeof(struct page)
  ARM64, Linux 6.x: 64 bytes per struct page

Memory overhead calculation:
  System RAM: 8 GB
  Page size:  4 KB = 4096 bytes
  Number of pages: 8 GB / 4 KB = 2,097,152 pages (2M pages)
  struct page array size: 2M × 64 bytes = 128 MB
  
  Overhead ratio: 128 MB / 8 GB = 1.5625%
  
  For larger systems:
    256 GB RAM → 4 GB of struct page overhead
    1 TB RAM   → 16 GB of struct page overhead
  
  vmemmap virtual address consumption:
    Same as struct page array size (1:1 virtual-to-struct mapping)

struct page size breakdown (64 bytes):
  flags (8 bytes):         page state flags
  union (varies):          16–32 bytes for page-type-specific data
  _refcount (4 bytes):     reference count
  _mapcount (4 bytes):     PTE reference count
  compound_head (8 bytes): compound page linkage
  Padding:                 alignment to 64-byte cacheline

NUMA considerations:
  NUMA node: stored in flags field (top bits)
  page_to_nid(page):  page→flags >> FLAGS_SHIFT & NODES_MASK
  Allows: reclaim decisions based on NUMA locality
  ARM64 server chips (Graviton, Neoverse): common NUMA topology
```

---

## 6. Interview Questions & Answers

**Q1: Why does virt_to_phys() not work for vmalloc addresses? What should you use instead?**

`virt_to_phys(ptr)` is implemented as `ptr - PAGE_OFFSET` on ARM64. This formula is a simple arithmetic offset that works ONLY for addresses in the **linear mapping** (direct map), where every physical address maps to exactly `PA + PAGE_OFFSET` in virtual address space.

**Why vmalloc is different**: `vmalloc()` allocates physical pages that may be **physically non-contiguous** and maps them into a contiguous virtual range (`VMALLOC_START` to `VMALLOC_END`) using the kernel's page tables (not the linear map). The virtual addresses in the vmalloc range are completely unrelated to `PAGE_OFFSET` arithmetic.

If you call `virt_to_phys(vmalloc_addr)`, you'll subtract `PAGE_OFFSET` from a vmalloc virtual address, getting a completely **wrong physical address** — it won't be the physical address of your vmalloc'd memory at all.

**Correct approach for vmalloc addresses**:
- `vmalloc_to_page(va)`: walk the kernel page tables to find the `struct page` for a vmalloc virtual address
- `vmalloc_to_pfn(va)`: same, returns PFN

For **vmalloc** addresses going to DMA: Don't. Use `kmalloc()` / `dma_alloc_coherent()` instead, which guarantee physically contiguous pages with correct DMA mapping.

---

## 7. Quick Reference

| Conversion | Function | Complexity |
|---|---|---|
| PFN → struct page | pfn_to_page(pfn) | O(1): vmemmap + pfn |
| struct page → PFN | page_to_pfn(page) | O(1): page - vmemmap |
| KVA → PA | virt_to_phys / __pa | O(1): subtract PAGE_OFFSET |
| PA → KVA | phys_to_virt / __va | O(1): add PAGE_OFFSET |
| KVA → struct page | virt_to_page | O(1): via __pa + pfn_to_page |
| struct page → KVA | page_address | O(1): via page_to_pfn + __va |
| vmalloc VA → struct page | vmalloc_to_page | O(1): page table walk |

| Address Range | virt_to_phys? | vmalloc_to_page? |
|---|---|---|
| Linear map (kmalloc, stack) | Yes | No |
| vmalloc space | NO (wrong!) | Yes |
| ioremap space | NO (wrong!) | No |
| vmemmap space | No | No |
