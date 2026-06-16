# vmalloc, vfree, and the vmalloc Region

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
vmalloc: allocate virtually contiguous but physically non-contiguous kernel memory
  
  Contrast with kmalloc:
    kmalloc: physically AND virtually contiguous (from buddy allocator)
             limited by buddy order (max ~4MB per allocation)
    vmalloc: virtually contiguous, physically scattered
             can allocate large sizes (theoretically up to VMALLOC_END - VMALLOC_START)
             slower (creates separate page table entries per 4KB page)

When to use vmalloc vs kmalloc:
  Use kmalloc when:
    - DMA operations (need physically contiguous)
    - Size < ~128KB (efficient from buddy/SLUB)
    - Performance critical (no extra page table overhead)
    - Passing to hardware that uses physical addresses
  
  Use vmalloc when:
    - Large allocation (> 1MB) where physical contiguity not needed
    - Loading kernel modules (module code into vmalloc space)
    - eBPF programs (JIT'd into vmalloc space)
    - Stack allocations for kernel threads (in some configs)
    - Large I/O buffers where size > buddy MAX_ORDER

ARM64 vmalloc region:
  VMALLOC_START: 0xFFFF_8000_0000_0000 (for 48-bit VA)
  VMALLOC_END:   0xFFFF_B000_0000_0000 (approximate)
  Size: ~48 TB of vmalloc space (48-bit VA ARM64)
  
  Each vmalloc allocation:
    - 1 guard page before the allocation (no-map guard)
    - N pages for the actual data (N = ceil(size/PAGE_SIZE))
    - 1 guard page after (no-map guard)
    Guards prevent write-through from one vmalloc allocation to another
```

---

## 2. vmalloc Implementation

```c
/* mm/vmalloc.c */

void *vmalloc(unsigned long size)
{
    return __vmalloc_node(size, 1, GFP_KERNEL, NUMA_NO_NODE, ...);
}

void *vzalloc(unsigned long size)  // vmalloc + zero
{
    return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_ZERO, ...);
}

void *vmalloc_user(unsigned long size)  // vmalloc for user copy (zeroed)
{
    return __vmalloc_node(size, SHMLBA, GFP_KERNEL | __GFP_ZERO, ...);
}

void *__vmalloc_node(unsigned long size, unsigned long align,
                     gfp_t gfp_mask, int node, ...)
{
    // 1. Allocate vm_struct (vmalloc area descriptor):
    struct vm_struct *area = get_vm_area_caller(size, VM_ALLOC, caller);
    // get_vm_area():
    //   Searches vmalloc_vmap_area_root (red-black tree of vmap_area)
    //   Find free virtual address range of size: size + guard pages
    //   VM_ALLOC flag: standard vmalloc
    //   VM_MAP: already have pages to map (used by vmap())
    //   VM_IOREMAP: for ioremap
    //   area->addr = start VA of this vmalloc region
    //   area->size = size + guard pages
    
    // 2. Allocate physical pages (from buddy, may be scattered):
    // ARM64: allocate one page at a time:
    for (i = 0; i < nr_pages; i++) {
        pages[i] = alloc_page(gfp_mask | __GFP_NOWARN);
        if (!pages[i]) goto fail;
    }
    
    // 3. Map pages into vmalloc VA range:
    map_vm_area(area, prot, pages):
        // For each page:
        //   Walk/create kernel page tables for this VA
        //   pgd_offset_k → pud_alloc → pmd_alloc → pte_alloc
        //   set_pte_at(init_mm, va, ptep, pte_entry)
        //   pte_entry = PA | AttrIndx_Normal | AP_EL1_RW | PXN | valid
        //   ARM64: Normal memory, inner-shareable, read-write kernel
    
    // 4. Flush TLB (not strictly needed for new mappings, but per-CPU sync):
    // ARM64: new mappings don't need TLB flush (no old entry to invalidate)
    
    return area->addr;
}

/* vm_struct tracks the vmalloc allocation: */
struct vm_struct {
    struct vm_struct    *next;
    void                *addr;           // VA start
    unsigned long       size;            // size including guard
    unsigned long       flags;           // VM_ALLOC, VM_MAP, VM_IOREMAP
    struct page         **pages;         // array of physical pages
    unsigned int        nr_pages;        // number of pages
    phys_addr_t         phys_addr;       // for ioremap (single PA)
    const void          *caller;         // allocation stack trace
};
```

---

## 3. vfree Implementation

```c
void vfree(const void *addr)
{
    struct vm_struct *area = find_vm_area(addr);
    // Lookup in vmap_area_root rb-tree: O(log N)
    
    // 1. Unmap from kernel page tables:
    remove_vm_area(addr):
        // Walk kernel page tables for this VA range
        // Clear each PTE: pte_clear()
        // Free PTE/PMD table pages if fully empty
    
    // 2. TLB flush: CRITICAL!
    //   Must flush BEFORE freeing physical pages
    //   Otherwise: another allocation might reuse PFN,
    //   and stale TLB entry could still map old PFN!
    //   ARM64: flush_tlb_kernel_range(start, end)
    //     → TLBI VAAE1IS, <addr> for each page
    //       (VAAE1IS: invalidate by VA, all ASID, EL1, inner-shareable)
    //     → DSB ISH (wait for TLB invalidation)
    
    // 3. Free physical pages:
    for (i = 0; i < area->nr_pages; i++) {
        __free_page(area->pages[i]);
        // Returns page to buddy allocator (order 0)
    }
    
    // 4. Free pages array:
    kvfree(area->pages);
    
    // 5. Free vm_struct:
    kfree(area);
}
```

---

## 4. vmap and vunmap

```
vmap(): map EXISTING pages into vmalloc VA space
  Unlike vmalloc() which allocates pages:
    vmalloc: alloc pages + map them
    vmap:    you provide pages, just maps them
  
  Use case: scatter-gather: have physically scattered pages, need contiguous VA
  
  void *vmap(struct page **pages, unsigned int count,
             unsigned long flags, pgprot_t prot):
    area = get_vm_area(count * PAGE_SIZE, flags)
    map_vm_area(area, prot, pages)
    return area->addr

  vunmap(addr):
    Similar to vfree but doesn't free the physical pages (you own them)
    Just unmaps the VA range + TLB flush

ioremap() is implemented via vmap-like path:
  ioremap(phys_addr, size):
    area = get_vm_area(size, VM_IOREMAP)
    // Map physical MMIO address range into vmalloc space
    // Uses Device memory attributes (nGnRnE or nGnRE)
    // NOT Normal memory!
    area->phys_addr = phys_addr
    ioremap_page_range(area->addr, phys_addr, size, pgprot_noncached)
    // pgprot_noncached on ARM64: AttrIndx=Device nGnRE, SH=NS
    return area->addr + (phys_addr & ~PAGE_MASK)
```

---

## 5. Lazy vmalloc TLB Flush

```
Performance optimization: lazy TLB flush for vmalloc areas

Problem: vfree must flush TLB on ALL CPUs (vmalloc is in TTBR1 = kernel space
         shared by all CPUs). For large vmalloc regions, this is expensive.

Lazy flush mechanism:
  Instead of immediate per-CPU IPI for TLB flush:
  Mark the vmalloc area as "pending flush" (lazy)
  When any CPU tries to use a VA in the vmalloc range:
    CPU encounters invalid PTE (was unmapped by vfree)
    Takes a kernel page fault
    vmalloc_fault() handler: checks if kernel page tables have mapping
    If kernel pgd has mapping but CPU's pgd doesn't: SYNC the entry
    (copy from swapper_pg_dir to this CPU's pgd)
    If no kernel mapping: real fault → BUG/oops

ARM64 and vmalloc_fault:
  ARM64: all CPUs share the SAME kernel page tables (swapper_pg_dir = TTBR1)
  Unlike x86 (which has per-CPU page tables):
    ARM64 TTBR1_EL1 points to the SAME swapper_pg_dir on all CPUs
    When vfree clears a PTE in swapper's page tables:
      The change is IMMEDIATELY visible to all CPUs (shared tables)
      BUT: TLB may still have old entry cached
    
  Therefore: ARM64 vmalloc TLB flush:
    flush_tlb_kernel_range(start, end):
      TLBI VAAE1IS, addr (for each page)  — inner-shareable = all CPUs
      DSB ISH                              — wait for all CPUs to complete
    This is a BROADCAST TLB flush: affects all CPUs simultaneously
    No per-CPU IPIs needed (unlike x86 which sends IPIs for TLB shootdown)
  
  ARM64 advantage: TLBI *IS instructions broadcast automatically
  x86 disadvantage: must send IPIs to remote CPUs for TLB flush
```

---

## 6. Interview Questions & Answers

**Q1: Can vmalloc addresses be used for DMA? Why or why not?**

Generally **no**, but with important nuances.

DMA typically requires **physically contiguous memory**. The DMA controller or device uses physical addresses, and it can only program a single base address + length for a transfer. If the buffer is physically non-contiguous (as vmalloc'd memory is), the DMA would read/write wrong data.

However, there are exceptions:
1. **IOMMU/SMMU**: if an IOMMU is present (common on ARM64 servers), it can create a contiguous IOVA (I/O Virtual Address) space mapped to scattered physical pages. In this case, `dma_map_single(dev, vmalloc_ptr, ...)` would work — BUT only if the DMA API supports it, and only after properly calling `vmalloc_to_page()` for each page to build the scatter-gather list.

2. **Scatter-gather DMA**: the driver can build an `sg_list` from vmalloc'd pages using `vmalloc_to_page()` and then use `dma_map_sg()`. Each sg entry maps one physically-contiguous chunk.

The fundamental issue: `dma_map_single()` expects a physically contiguous buffer and uses `virt_to_phys()` which doesn't work for vmalloc addresses. The safe approach is always to use `kmalloc()` or `dma_alloc_coherent()` for DMA buffers.

---

## 7. Quick Reference

| API | Physically Contiguous? | Max Size | Use Case |
|---|---|---|---|
| kmalloc() | YES | ~128KB (SLUB) / 4MB (buddy) | Small/medium kernel data |
| alloc_pages() | YES | 4MB (order 10) | Page-level allocation |
| vmalloc() | NO | ~TB (VA limited) | Large, non-DMA allocations |
| vmap() | NO (your pages) | ~TB | Map existing pages |
| ioremap() | NO (1 MMIO PA) | ~TB | MMIO device memory |

| vmalloc Guard Pages | Purpose |
|---|---|
| 1 guard page before | Detect underflow |
| 1 guard page after | Detect overflow |
| Access: fault → oops | Catches out-of-bounds access |
