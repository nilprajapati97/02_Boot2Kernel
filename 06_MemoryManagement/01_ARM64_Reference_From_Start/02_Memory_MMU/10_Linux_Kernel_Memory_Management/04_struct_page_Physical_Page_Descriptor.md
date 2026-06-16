# struct page: Physical Page Descriptor

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
struct page: kernel's descriptor for every PHYSICAL page frame.
  One struct page exists for EVERY physical page in the system.
  
  If system has 4GB RAM → 1 million 4KB pages → 1 million struct page instances
  
  struct page size: ~64 bytes (highly optimized union)
  Memory overhead: 4GB RAM → 1M × 64B = 64MB for struct page array
  (1.5% overhead — worth it for complete physical memory management)

struct page array (vmemmap):
  On ARM64: virtual contiguous array at VMEMMAP_START
  vmemmap: base address of the struct page array
  
  pfn_to_page(pfn): vmemmap + pfn (pointer arithmetic on struct page[])
  page_to_pfn(page): page - vmemmap (offset from base)
  
  ARM64 vmemmap address: 0xFFFF_FFF8_0000_0000 (for 48-bit VA)
  Each entry: sizeof(struct page) = 64 bytes
  For PA 0x0 → page at vmemmap[0]
  For PA 0x1000 (PFN=1) → page at vmemmap[1] = vmemmap + 64 bytes

Why vmemmap (sparse mapping)?
  Not all physical addresses have RAM behind them (MMIO holes, NUMA gaps)
  Sparse model: allocate struct page only for PRESENT memory regions
  vmemmap: virtual-to-struct-page is O(1) (pointer arithmetic)
```

---

## 2. struct page Union Layout

```c
/* include/linux/mm_types.h (simplified) */
struct page {
    unsigned long   flags;       // Page flags (PageDirty, PageUptodate, etc.)
    
    union {
        /* For anonymous/file pages in page cache: */
        struct {
            struct list_head    lru;        // LRU list (active, inactive)
            struct address_space *mapping;  // page cache tree (file) or anon_vma (anon)
            pgoff_t             index;      // offset in file (or mm offset for anon)
            unsigned long       private;    // used by buffer_heads, etc.
        };
        
        /* For slab allocator pages: */
        struct {
            union {
                struct list_head    slab_list;   // slab's freelist
                struct {
                    struct page *next;
                    int pages;         // number of pages in slab
                };
            };
            struct kmem_cache   *slab_cache;  // which SLUB cache owns this page
            void                *freelist;    // first free object on this slab page
            union {
                void    *s_mem;    // first allocated object
                unsigned long counters;  // pack inuse, frozen bits
            };
        };
        
        /* For buddy allocator free pages: */
        struct {
            unsigned long private;  // buddy order (0–10 = 4KB to 4MB)
            struct page *buddy;     // buddy pair pointer
        };
        
        /* For huge pages: */
        struct {
            unsigned long compound_head;   // bit 0 = 1 for tail pages
            unsigned char compound_order;  // order of the huge page
            unsigned char compound_mapcount;
        };
        
        /* For device/vmalloc pages: */
        struct {
            struct dev_pagemap *pgmap;
            void               *zone_device_data;
        };
        
    }; /* end of big union */
    
    /* Reference count: */
    atomic_t        _refcount;   // page reference count (>0 = in use)
    
    /* NUMA node + zone membership: */
    unsigned int    _mapcount;   // number of page table entries pointing here (rmap)
    
    /* Tail page compound link: */
    unsigned long   compound_head;  // for compound pages (huge pages)

#ifdef CONFIG_MEMCG
    struct mem_cgroup *mem_cgroup;   // memory cgroup this page belongs to
#endif
};
```

---

## 3. Page Flags

```
flags field (unsigned long, 64-bit on ARM64):

Layout:
  [bits 0-20]: page flags (PG_* bits)
  [bits 21-25]: zone index (which NUMA zone: ZONE_DMA, ZONE_NORMAL, etc.)
  [bits 26-...]: NUMA node ID

Important flags (include/linux/page-flags.h):

PG_locked (bit 0):     page is locked (during I/O)
PG_referenced (bit 2): recently accessed (for LRU aging)
PG_uptodate (bit 3):   contents are valid (read from disk completed)
PG_dirty (bit 4):      page has been modified (not written back to disk)
PG_lru (bit 5):        page is on the LRU list
PG_active (bit 6):     page is on the active LRU list
PG_slab (bit 7):       page is managed by SLUB allocator
PG_reserved (bit 14):  page reserved (not for general use, e.g., BIOS area)
PG_private (bit 11):   page->private is used (e.g., buffer heads)
PG_writeback (bit 8):  page is being written back to disk
PG_compound (bit 15):  this page is part of a compound (huge) page
PG_swapbacked (bit 10): page is backed by swap (anonymous page)
PG_unevictable (bit 18): page cannot be reclaimed (mlock'd, ramfs, etc.)

ARM64-specific:
  PG_dcache_clean:    D-cache has been cleaned for this page
  (Used to track cache maintenance for DMA operations)

Test/Set/Clear helpers:
  PageDirty(page):    test_bit(PG_dirty, &page->flags)
  SetPageDirty(page): set_bit(PG_dirty, &page->flags)
  ClearPageDirty(page): clear_bit(PG_dirty, &page->flags)
  
  test_and_set_bit: atomic bit operation (used for PG_locked)
  lock_page(page):  wait_on_bit_lock(PG_locked) → sleeps if locked
  unlock_page(page): clear PG_locked, wake up waiters
```

---

## 4. _refcount and _mapcount

```
_refcount: general reference count for the page
  >0: page is in use (someone holds a reference)
  =0: page is free (can be given to buddy allocator)
  
  get_page(page):  page_ref_inc(page) — increment refcount
  put_page(page):  page_ref_dec_return() — decrement; if 0: free page
  
  Who holds references:
    Page cache (mapping->i_pages): 1 reference per cached page
    User-space mapping (PTE): 1 reference per page in PTE (via _mapcount)
    pin_user_pages() / get_user_pages(): 1 reference per pinned page
    DMA: dma_pin_pages() keeps refcount elevated while DMA in progress

_mapcount: number of PAGE TABLE ENTRIES pointing to this page
  = -1: page not mapped in any PTE
  = 0:  page mapped in exactly ONE PTE (most common)
  = 1:  page mapped in TWO PTEs (shared, fork without COW yet)
  = N:  page mapped in N+1 PTEs total
  
  page_mapcount(page) = atomic_read(&page->_mapcount) + 1
  
  _mapcount is used for REVERSE MAPPING (rmap):
    If _mapcount > 0: there are PTEs pointing to this page
    rmap: given a page, find all PTEs pointing to it
    Used by: page reclaim (need to unmap all PTEs before freeing)
    
  ARM64 rmap:
    For anonymous pages: page->mapping → struct anon_vma
    anon_vma_chain: links between anon_vma and VMAs
    reverse_map_walk() → finds all PTEs pointing to page
    ptep_clear_flush() → removes PTE + TLB flush

struct page is SHARED between uses (union):
  A page used by SLUB: slab_cache, freelist (union members)
  A page in page cache: mapping, index (union members)
  A free page in buddy: private = order (union member)
  These NEVER overlap (page has ONE owner at a time)
```

---

## 5. pfn_to_page and page_to_pfn

```
PFN (Page Frame Number): page number in the physical address space
  PFN = physical_address >> PAGE_SHIFT (>> 12 for 4KB pages)
  
  PA 0x00000000 = PFN 0
  PA 0x00001000 = PFN 1
  PA 0x40000000 = PFN 0x40000 (262144)

pfn_to_page(pfn):
  ARM64 vmemmap:
    return vmemmap + pfn
    (vmemmap = starting address of struct page array)
    
  This is O(1): simple pointer arithmetic
  Assumes: vmemmap is the base of a contiguous struct page array

page_to_pfn(page):
  return page - vmemmap
  
  Also O(1): pointer subtraction

virt_to_page(kaddr):
  kernel virtual address → struct page
  = pfn_to_page(virt_to_pfn(kaddr))
  = pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
  
  Only valid for: kernel linear mapped addresses (direct mapping)
  NOT valid for: vmalloc addresses, ioremap addresses

page_address(page):
  struct page → kernel virtual address
  = __va(page_to_pfn(page) << PAGE_SHIFT)
  = phys_to_virt(page_to_phys(page))
  
  Returns the linear-mapped kernel virtual address of the page

ARM64 physical/virtual conversions:
  __pa(kva) = (kva) - PAGE_OFFSET  (for linear mapped kernel VA)
  __va(pa)  = (pa)  + PAGE_OFFSET  (for linear mapped kernel PA)
  
  PAGE_OFFSET: start of kernel linear mapping (TTBR1_EL1 base)
               ARM64: 0xFFFF_0000_0000_0000 (for 48-bit VA, 4-level)
  
  phys_to_virt(pa):  return (void *)((pa) + PAGE_OFFSET)
  virt_to_phys(kva): return ((unsigned long)(kva)) - PAGE_OFFSET
  
  Limitations:
    virt_to_phys only valid for linear-mapped kernel addresses
    vmalloc addresses: need vmalloc_to_pfn() or follow page tables
```

---

## 6. Interview Questions & Answers

**Q1: struct page is defined as a union of many different structs. How does the kernel know which union member is currently active for a given page?**

The kernel knows which union member is active based on **page flags** (`page->flags`) and the **page's current owner**:

- If `PG_slab` is set → the page belongs to a SLUB slab → `slab_cache`, `freelist` union members are valid
- If `page->mapping != NULL` and `PG_slab` is not set → page is in the page cache or has an `anon_vma` → the page cache union (`mapping`, `index`, `lru`) is valid
- If the page is free (not in use, `PG_locked`, `PG_slab`, etc. all clear) → buddy allocator owns it → `page->private` contains the buddy order
- If `PageCompound(page)` is set → compound page → `compound_head`, `compound_order` are valid

This is an example of **tagged union** usage in the kernel without a discriminator field — the discriminator IS the page flags and the context of who owns the page. There's no type-safety at the C level; the kernel relies on invariants and conventions (e.g., the buddy allocator always sets `PG_buddy` before interpreting the `private` field; SLUB sets `PG_slab` before using `slab_cache`).

---

## 7. Quick Reference

| Conversion | Function | Notes |
|---|---|---|
| PFN → struct page | pfn_to_page(pfn) | vmemmap + pfn |
| struct page → PFN | page_to_pfn(page) | page - vmemmap |
| kva → PA | __pa(kva) or virt_to_phys | Only linear map |
| PA → kva | __va(pa) or phys_to_virt | Only linear map |
| kva → struct page | virt_to_page(kva) | Via __pa then pfn_to_page |
| struct page → kva | page_address(page) | Via page_to_pfn then __va |

| Flag | Meaning |
|---|---|
| PG_dirty | Modified, not written back |
| PG_uptodate | Valid data (I/O completed) |
| PG_locked | Under I/O (sleep if trying to lock) |
| PG_slab | Owned by SLUB allocator |
| PG_active | On active LRU list |
| PG_lru | On any LRU list |
| PG_unevictable | Cannot be reclaimed |
| PG_compound | Part of compound/huge page |
