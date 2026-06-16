# phys_to_virt, __pa/__va, and Address Translation Macros

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
ARM64 kernel operates in two distinct virtual address ranges:
  TTBR0_EL1: user-space VA (0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF)
  TTBR1_EL1: kernel VA    (0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF)

Kernel VA space layout (48-bit VA, 4KB pages):
  
  [PAGE_OFFSET = 0xFFFF_0000_0000_0000]
       ↓
  Linear Mapping (Direct Map):
    Every physical byte has a corresponding kernel virtual address
    KVA = PA + PAGE_OFFSET (when PHYS_OFFSET = 0)
    (or: KVA = PA - PHYS_OFFSET + PAGE_OFFSET)
    
    This makes PA↔KVA conversions trivial arithmetic.
    Most kernel data (struct page, kmalloc, stack) lives here.
  
  [VMALLOC_START = 0xFFFF_8000_0000_0000]
       ↓  
  vmalloc/ioremap region:
    Physically non-contiguous or device memory
    Mapped with explicit page tables
    __pa() does NOT work here
  
  [VMEMMAP_START = ~0xFFFF_FFF8_0000_0000]
       ↓
  vmemmap: struct page array
  
  [kimage_vaddr = near 0xFFFF_FFFF_8000_0000]
       ↓
  Kernel image: .text, .rodata, .data, .bss
    Mapped with kimage_voffset (may differ from PAGE_OFFSET)
    KASLR changes kimage_vaddr at boot

Key insight:
  __pa()/__va() are valid ONLY for linear-mapped addresses
  For kernel image (with KASLR): use kaslr-aware __pa_symbol() for symbols
  For vmalloc: use vmalloc_to_page()
```

---

## 2. ARM64-Specific Address Translation Macros

```c
/* arch/arm64/include/asm/memory.h */

/* Physical offset of the start of RAM */
extern s64 memstart_addr;
#define PHYS_OFFSET     ({ memstart_addr; })
/* memstart_addr: set at boot by arm64_memblock_init()
   Typical ARM64: memstart_addr = 0x40000000 (1GB, some boards)
   Or:           memstart_addr = 0x00000000 (if RAM starts at PA 0)
   
   On UEFI-booted ARM64 servers: may be any value set by firmware
*/

/* PAGE_OFFSET: start of kernel virtual linear mapping */
#define PAGE_OFFSET     (UL(0) - (UL(1) << VA_BITS))
/* VA_BITS=48: PAGE_OFFSET = 0xFFFF_0000_0000_0000
   VA_BITS=52: PAGE_OFFSET = 0xFFF0_0000_0000_0000 (FEAT_LVA)
*/

/* __pa(x): kernel VA → physical address */
#define __pa(x)         __phys_addr_nodebug((unsigned long)(x))
#define __phys_addr_nodebug(x)    ((x) - PAGE_OFFSET + PHYS_OFFSET)
/* = (x) - 0xFFFF_0000_0000_0000 + memstart_addr */

/* __va(x): physical address → kernel VA */
#define __va(x)         ((void *)((unsigned long)(x) + PAGE_OFFSET - PHYS_OFFSET))
/* = x + 0xFFFF_0000_0000_0000 - memstart_addr */

/* __pa_symbol(x): for kernel symbols (handles KASLR properly) */
#define __pa_symbol(x)  __phys_addr_symbol(RELOC_HIDE((unsigned long)(x), 0))
/* Must use for: kernel .text/.data symbols, not general use */
/* Handles: kimage_voffset ≠ PAGE_OFFSET (when KASLR enabled) */

/* kimage_voffset: offset between kernel image VA and linear map VA */
extern u64 kimage_voffset;
/* Set in head.S during kernel boot (before MMU enable) */
/* With KASLR: kimage_voffset varies each boot */
/* Without KASLR: kimage_voffset == PAGE_OFFSET */

/* kernel virtual ↔ physical for kernel TEXT/DATA: */
#define __pa_symbol(sym)   (((unsigned long)(sym)) - kimage_voffset)
#define lm_alias(x)        ((typeof(x))__va(__pa_symbol(x)))
```

---

## 3. Linear Mapping Setup

```
ARM64 kernel boot process (arch/arm64/kernel/head.S):

1. CPU boots at EL2 or EL1
2. Before MMU: CPU running at physical addresses
3. Setup identity mapping (PA = VA): only for boot code
4. Create kernel page tables:
   a. create_pgd_mapping(lm_early_pgd, ...):
      Maps [PHYS_OFFSET, PHYS_OFFSET + memblock_end) →
           [PAGE_OFFSET, PAGE_OFFSET + (memblock_end - PHYS_OFFSET))
      Uses section (2MB) or page (4KB) granule
      Default attributes: NORMAL memory, inner shareable, read-write, no-exec

5. Enable MMU (write SCTLR_EL1.M = 1)
   CPU now running at kernel VA (PAGE_OFFSET range)

6. map_kernel():
   Maps kernel image with proper attributes:
     .text:   r-xp (AP=RO, PXN=0, UXN=1)
     .rodata: r--p (AP=RO, PXN=1, UXN=1)
     .data:   rw-p (AP=RW, PXN=1, UXN=1)
     .bss:    rw-p (AP=RW, PXN=1, UXN=1)
   Kernel image VA: kimage_vaddr (randomized if KASLR)

Linear map ARM64 TLB optimization:
  Large pages used wherever possible:
    1GB block descriptors (PUD) for large NUMA memory banks
    2MB block descriptors (PMD) for most RAM
    4KB pages only at boundaries or for special regions
  
  Effect: fewer TLB entries needed to cover all RAM
  ARM64 PMD (2MB) descriptor: bit[0]=1, bit[1]=0 (block), vs
  ARM64 PTE (4KB) descriptor: bit[0]=1, bit[1]=1 (page)
```

---

## 4. KASLR and __pa_symbol

```
KASLR (Kernel Address Space Layout Randomization):
  Linux 4.6+: ARM64 supports KASLR
  At each boot: kernel image loaded at randomized physical AND virtual address
  
  Physical randomization: kernel loaded at randomized PA (from UEFI)
  Virtual randomization: kimage_vaddr = random offset in [PAGE_OFFSET, ...]
  
  kimage_voffset = kimage_vaddr - (phys_addr of kernel image)
  
  Without KASLR:
    kimage_vaddr = PAGE_OFFSET + (phys_kernel_start - PHYS_OFFSET)
    → same as linear mapping
    → kimage_voffset = PAGE_OFFSET
    → __pa_symbol(sym) = __pa(sym) (identical)
  
  With KASLR:
    kimage_vaddr ≠ PAGE_OFFSET + (phys_kernel_start - PHYS_OFFSET)
    → kimage_voffset ≠ PAGE_OFFSET
    → __pa_symbol(sym) ≠ __pa(sym) ← critical difference!
  
  Bug pattern:
    // WRONG with KASLR:
    unsigned long pa = __pa(kernel_function);  
    // kernel_function is in .text, kimage VA region
    // __pa() subtracts PAGE_OFFSET, giving WRONG physical address!
    
    // CORRECT with KASLR:
    unsigned long pa = __pa_symbol(kernel_function);
    // Uses kimage_voffset, gives correct physical address

fixmap:
  Fixed virtual address mappings (not linear, not vmalloc)
  Defined in arch/arm64/include/asm/fixmap.h
  FIX_TEXT_POKE0, FIX_EARLYCON_MEM_BASE, etc.
  Used for early-boot mappings before vmalloc is initialized
  VA is known at compile time, PA set at runtime via set_fixmap()
  
  __fix_to_virt(idx): compile-time fixed VA
  set_fixmap(idx, phys): maps phys PA to fixed VA idx
```

---

## 5. Common Usage Patterns

```c
/* Pattern 1: kmalloc'd data → DMA address */
void *buf = kmalloc(4096, GFP_KERNEL);
dma_addr_t dma = dma_map_single(dev, buf, 4096, DMA_TO_DEVICE);
// NOT: dma = virt_to_phys(buf)  ← doesn't handle IOMMU/SMMU

/* Pattern 2: struct page → physical address for page tables */
struct page *page = alloc_page(GFP_KERNEL);
phys_addr_t pa = page_to_phys(page);
// page_to_phys = page_to_pfn(page) << PAGE_SHIFT
// Install in page table: pte = pfn_pte(page_to_pfn(page), prot)
// ARM64: TTBR0 = pa (physical!)

/* Pattern 3: physical address in device tree / MMIO → ioremap */
phys_addr_t mmio_base = 0xFE000000;  // from device tree
void __iomem *regs = ioremap(mmio_base, 0x1000);
// Creates vmalloc mapping for device MMIO
// NOT in linear map (device memory: nGnRE, strongly-ordered)
// Must use: readl/writel (memory barriers included)

/* Pattern 4: get physical address of kernel symbol */
extern char _stext[];  // start of kernel .text
phys_addr_t text_phys = __pa_symbol(_stext);
// Use __pa_symbol (not __pa) for kernel image symbols

/* Pattern 5: virt_to_phys for driver private allocation */
struct drv_data *drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
phys_addr_t drv_phys = virt_to_phys(drv);
// OK: devm_kzalloc uses kmalloc (linear mapped)
// NOT OK: if drv were allocated with vmalloc

/* Pattern 6: page_address for bio data access */
struct bio *bio = bio_alloc(GFP_KERNEL, 1);
struct page *page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
bio_add_page(bio, page, PAGE_SIZE, 0);
// Kernel accesses page data via:
void *data = kmap_local_page(page);
// On ARM64: kmap_local_page = page_address (no HIGHMEM, always linear mapped)
```

---

## 6. Interview Questions & Answers

**Q1: A kernel driver has a pointer obtained from `ioremap()`. The driver calls `virt_to_phys()` on this pointer. What happens, and what is the correct approach?**

**What happens**: `virt_to_phys(ioremap_ptr)` computes `(unsigned long)ioremap_ptr - PAGE_OFFSET`. But `ioremap_ptr` is in the vmalloc region (virtual addresses starting at `VMALLOC_START`), NOT in the linear mapping. The subtraction of `PAGE_OFFSET` from a vmalloc address gives a completely **wrong physical address** — typically a very large number or a negative number interpreted as unsigned, which does not correspond to any real physical address.

**Correct approach**: For `ioremap()` mappings, you simply don't need to convert back to physical address because you already have it (you passed the MMIO physical address TO `ioremap()`). Keep the original physical address around if you need it.

If you absolutely must extract the physical address from an `ioremap()` pointer (not recommended), you'd need to walk the kernel page tables:
```c
pgd_t *pgd = pgd_offset_k(va);
// ... walk PUD, PMD, PTE ...
phys = pte_val(*pte) & PAGE_MASK;
```

But the right design is: when you call `ioremap(phys, size)`, save the `phys` address separately if you need it later.

---

## 7. Quick Reference

| Macro/Function | Input | Output | Valid For |
|---|---|---|---|
| `__pa(x)` | KVA (linear map) | PA | kmalloc, stack, global data |
| `__va(x)` | PA | KVA (linear map) | Any PA in RAM |
| `virt_to_phys(x)` | KVA (linear map) | PA | Same as `__pa()` |
| `phys_to_virt(x)` | PA | KVA (linear map) | Same as `__va()` |
| `__pa_symbol(x)` | Kernel symbol VA | PA | .text, .data, .bss symbols |
| `page_to_phys(p)` | struct page * | PA | Any struct page |
| `phys_to_page(p)` | PA | struct page * | Any PA in RAM |
| `vmalloc_to_pfn(v)` | vmalloc KVA | PFN | vmalloc regions |

| ARM64 Address | virt_to_phys OK? | Notes |
|---|---|---|
| kmalloc (slab) | Yes | Linear mapped |
| kzalloc (slab) | Yes | Linear mapped |
| vmalloc | NO | Use vmalloc_to_pfn() |
| ioremap | NO | Keep PA from ioremap() call |
| Kernel symbol (no KASLR) | Yes | __pa() works |
| Kernel symbol (KASLR on) | NO | Use __pa_symbol() |
| stack (kthread) | Yes | Linear mapped |
