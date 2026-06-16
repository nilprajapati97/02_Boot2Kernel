# Linux Kernel Memory APIs

---

## 1. Basic Allocation (Most Common)

```c
kmalloc()
kzalloc()
kfree()
```

---

## 2. Large Memory Allocation

```c
vmalloc()
vfree()
```

---

## 3. Slab Allocator (Efficient Object Allocation)

```c
kmem_cache_create()
kmem_cache_alloc()
kmem_cache_free()
kmem_cache_destroy()
```

---

## 4. Page-Level Allocation

```c
alloc_pages()
__get_free_pages()
free_pages()
```

---

## 5. User Space ↔ Kernel Space

```c
copy_to_user()
copy_from_user()
```

---

## 6. Memory Mapping

```c
mmap()           /* via file operations */
remap_pfn_range()
```

---

## 7. DMA Memory Allocation

```c
dma_alloc_coherent()
dma_free_coherent()
```



On the ARM64 Linux kernel, **Memory Management (MM)** and **Memory Management Unit (MMU)** APIs span several subsystems, including page allocation, virtual memory management, page tables, DMA mapping, cache/TLB management, and address translation.

## 1. Physical Memory Allocation APIs

### Page Allocator APIs

Used for allocating physical pages.

```c
struct page *alloc_pages(gfp_t gfp_mask, unsigned int order);
struct page *alloc_page(gfp_t gfp_mask);
void __free_pages(struct page *page, unsigned int order);
void free_page(unsigned long addr);
void free_pages(unsigned long addr, unsigned int order);
```

Example:

```c
struct page *page = alloc_page(GFP_KERNEL);
```

---

### Slab/SLUB Allocator APIs

```c
void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void *kcalloc(size_t n, size_t size, gfp_t flags);
void kfree(const void *objp);
```

Example:

```c
char *buf = kzalloc(1024, GFP_KERNEL);
```

---

### vmalloc APIs

Allocates virtually contiguous memory.

```c
void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);
void vfree(const void *addr);
```

Example:

```c
void *buf = vmalloc(1 * 1024 * 1024);
```

---

## 2. User Space Mapping APIs

### mmap Related APIs

```c
int remap_pfn_range(struct vm_area_struct *vma,
                    unsigned long addr,
                    unsigned long pfn,
                    unsigned long size,
                    pgprot_t prot);

vm_fault_t vmf_insert_page(struct vm_area_struct *vma,
                           unsigned long addr,
                           struct page *page);
```

Used in driver mmap implementations.

---

## 3. Page Table APIs

ARM64 uses:

```
PGD -> P4D -> PUD -> PMD -> PTE
```

### Page Table Traversal

```c
pgd_t *pgd_offset(struct mm_struct *mm, unsigned long addr);
p4d_t *p4d_offset(pgd_t *pgd, unsigned long addr);
pud_t *pud_offset(p4d_t *p4d, unsigned long addr);
pmd_t *pmd_offset(pud_t *pud, unsigned long addr);
pte_t *pte_offset_kernel(pmd_t *pmd, unsigned long addr);
```

Example:

```c
pgd_t *pgd = pgd_offset(current->mm, va);
```

---

### PTE Manipulation

```c
pte_t pfn_pte(unsigned long pfn, pgprot_t prot);
void set_pte(pte_t *ptep, pte_t pte);
pte_t pte_mkdirty(pte_t pte);
pte_t pte_mkwrite(pte_t pte);
```

---

## 4. Virtual-to-Physical Translation APIs

### Kernel Address Translation

```c
phys_addr_t virt_to_phys(volatile void *address);
void *phys_to_virt(phys_addr_t address);
```

Example:

```c
phys_addr_t pa = virt_to_phys(buf);
```

---

### Page Translation

```c
struct page *virt_to_page(const void *kaddr);
void *page_address(const struct page *page);
unsigned long page_to_pfn(const struct page *page);
struct page *pfn_to_page(unsigned long pfn);
```

---

## 5. DMA Mapping APIs

Very important in ARM64 drivers.

### Streaming DMA

```c
dma_addr_t dma_map_single(struct device *dev,
                          void *cpu_addr,
                          size_t size,
                          enum dma_data_direction dir);

void dma_unmap_single(struct device *dev,
                      dma_addr_t dma_addr,
                      size_t size,
                      enum dma_data_direction dir);
```

---

### Coherent DMA

```c
void *dma_alloc_coherent(struct device *dev,
                         size_t size,
                         dma_addr_t *dma_handle,
                         gfp_t flag);

void dma_free_coherent(struct device *dev,
                       size_t size,
                       void *cpu_addr,
                       dma_addr_t dma_handle);
```

---

## 6. ARM64 Cache Maintenance APIs

Located in:

```text
arch/arm64/include/asm/cacheflush.h
```

Common APIs:

```c
flush_cache_all();
flush_dcache_page(struct page *page);
invalidate_dcache_range(unsigned long start,
                        unsigned long end);
```

Driver developers usually use DMA APIs instead of direct cache maintenance.

---

## 7. TLB Management APIs

Located in:

```text
arch/arm64/include/asm/tlbflush.h
```

### Common APIs

```c
flush_tlb_all();
flush_tlb_mm(struct mm_struct *mm);
flush_tlb_page(struct vm_area_struct *vma,
               unsigned long addr);
flush_tlb_range(struct vm_area_struct *vma,
                unsigned long start,
                unsigned long end);
```

---

## 8. I/O Memory Mapping APIs (MMU Mappings)

Used to map device physical addresses.

```c
void __iomem *ioremap(resource_size_t phys_addr,
                      unsigned long size);

void __iomem *ioremap_wc(resource_size_t phys_addr,
                         unsigned long size);

void iounmap(void __iomem *addr);
```

Example:

```c
regs = ioremap(0x10000000, 0x1000);
```

---

## 9. MM Structure APIs

### Current Process Memory

```c
current->mm
```

Important fields:

```c
struct mm_struct {
    pgd_t *pgd;
    atomic_t mm_users;
    unsigned long total_vm;
    unsigned long locked_vm;
};
```

---

### VMA Traversal

```c
struct vm_area_struct *vma;
```

Find VMA:

```c
vma = find_vma(mm, address);
```

---

## 10. ARM64 Specific MMU APIs

### TTBR Access

ARM64 MMU uses:

* TTBR0_EL1 → User page tables
* TTBR1_EL1 → Kernel page tables

Kernel helpers:

```c
read_sysreg(ttbr0_el1);
write_sysreg(val, ttbr0_el1);

read_sysreg(ttbr1_el1);
write_sysreg(val, ttbr1_el1);
```

---

### Translation Helpers

```c
__pa(x)
__va(x)
```

Example:

```c
phys_addr_t pa = __pa(kernel_addr);
void *va = __va(pa);
```

---

## 11. Memory Attribute APIs

ARM64 page protection APIs:

```c
pgprot_noncached(prot);
pgprot_writecombine(prot);
pgprot_device(prot);
```

Used when creating mappings:

```c
remap_pfn_range(vma,
                vma->vm_start,
                pfn,
                size,
                pgprot_noncached(vma->vm_page_prot));
```

---

## 12. Useful Debug APIs

### Dump Page Information

```c
dump_page(struct page *page, const char *reason);
```

### Memory Statistics

```c
si_meminfo(struct sysinfo *val);
```

### Page Table Dump

Enable:

```text
CONFIG_PTDUMP_DEBUGFS
```

Then:

```bash
cat /sys/kernel/debug/kernel_page_tables
```

---

## APIs Frequently Asked in Linux Driver/BSP Interviews (8+ Years)

1. `kmalloc()`, `kzalloc()`, `vmalloc()`
2. `alloc_pages()`
3. `virt_to_phys()`, `phys_to_virt()`
4. `virt_to_page()`, `page_to_pfn()`
5. `dma_alloc_coherent()`
6. `dma_map_single()`
7. `ioremap()`, `iounmap()`
8. `remap_pfn_range()`
9. `find_vma()`
10. `flush_tlb_*()`
11. `flush_dcache_page()`
12. `pgd_offset()`, `pud_offset()`, `pmd_offset()`, `pte_offset_kernel()`
13. `set_pte()`
14. `__pa()`, `__va()`
15. `read_sysreg()` / `write_sysreg()` for TTBR and MMU register access

For Linux Device Driver and BSP interviews, it's especially valuable to understand how these APIs fit into the ARM64 address translation flow:

VA \rightarrow PGD \rightarrow P4D \rightarrow PUD \rightarrow PMD \rightarrow PTE \rightarrow PA

and how TLBs cache these translations to avoid repeated page-table walks.

