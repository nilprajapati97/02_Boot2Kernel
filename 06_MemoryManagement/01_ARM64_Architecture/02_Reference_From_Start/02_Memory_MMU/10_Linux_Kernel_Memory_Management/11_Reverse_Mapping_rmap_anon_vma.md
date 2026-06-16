# Reverse Mapping (rmap) and anon_vma

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Forward mapping: VA → page table → PA (what the hardware does)
Reverse mapping (rmap): PA (struct page*) → all PTEs that map this page

Why reverse mapping?
  Page reclaim: kernel wants to free a physical page
    → Must unmap it from ALL page tables pointing to it first
    → Without rmap: must scan ALL page tables of ALL processes (O(total_pages))
    → With rmap: given struct page, directly find all PTEs (much faster)
  
  Memory migration (NUMA balancing, memory compaction):
    Move page from one physical address to another
    → Must update ALL PTEs pointing to old address

  Hardware poison handling (ECC memory error):
    Physical page is faulty → must unmap it from all processes

Two rmap mechanisms:
  1. File pages: rmap via address_space (mapping->i_mmap interval tree)
     Each file page: page->mapping = struct address_space
     page->index = file offset
     address_space->i_mmap: interval tree of all VMAs mapping this file
  
  2. Anonymous pages: rmap via anon_vma
     Each anon page: page->mapping = struct anon_vma (with bit 0 set as tag)
     page->index = virtual address / PAGE_SIZE
     anon_vma: tree of all VMAs that may contain this page
```

---

## 2. anon_vma Structure

```c
/* include/linux/rmap.h */

struct anon_vma {
    struct anon_vma     *root;          // root of the anon_vma tree
    struct rw_semaphore  rwsem;         // lock protecting this anon_vma
    atomic_t             refcount;      // reference count
    unsigned long        num_children;  // number of child anon_vmas (from fork)
    unsigned long        num_active_vmas; // number of VMAs actively using this
    struct anon_vma     *parent;        // parent anon_vma (NULL for root)
    struct rb_root_cached rb_root;      // rb-tree of anon_vma_chain objects
};

struct anon_vma_chain {
    struct vm_area_struct   *vma;       // the VMA this chain node belongs to
    struct anon_vma         *anon_vma;  // the anon_vma being linked
    struct list_head         same_vma;  // list of all avc's for one VMA
    struct rb_node           rb;        // node in anon_vma->rb_root
    unsigned long            rb_subtree_last; // optimization for rb subtree
};

/* One anon_vma per anonymous VMA (initially) */
/* One anon_vma_chain per (VMA, anon_vma) pair */
/* After fork: VMA is linked to multiple anon_vmas (parent + child chains) */

struct page {
    // ...
    union {
        struct {
            struct address_space *mapping;
            // For anonymous pages: mapping = (void*)anon_vma | 0x1
            //   bit 0 set = tag indicating anon_vma (not address_space)
            //   to distinguish: PageAnon(page) checks mapping's bit 0
            pgoff_t index;    // = VA >> PAGE_SHIFT (virtual page number)
        };
    };
};
```

---

## 3. anon_vma Setup and fork()

```
Initial VMA creation (first anonymous page fault):
  do_anonymous_page():
    → page_add_new_anon_rmap(page, vma, addr)
      → __page_set_anon_rmap(page, vma, addr, exclusive=true)
        → if (!vma->anon_vma) anon_vma_prepare(vma)
          Allocate anon_vma, allocate anon_vma_chain
          chain->vma = vma; chain->anon_vma = av
          Insert chain into av->rb_root
          vma->anon_vma = av
        → page->mapping = (struct address_space *)((unsigned long)av | 0x1)
        → page->index = linear_page_index(vma, addr)

fork() (anon_vma_fork):
  1. Allocate new anon_vma for child (avc)
  2. "Parent chain": anon_vma_chain from child VMA → parent's anon_vma
     Inserted into parent's anon_vma->rb_root
  3. "Child chain": anon_vma_chain from child VMA → child's new anon_vma
     Inserted into child's anon_vma->rb_root
  
  Result: child VMA is in BOTH parent's anon_vma rb_tree AND child's
  
  Why? Anonymous pages from parent (before fork) have page->mapping pointing
  to the PARENT's anon_vma. After fork, those pages are shared between
  parent and child. To find child's PTEs for those pages, we need the
  child VMA to be in the parent's anon_vma's rb_tree.
  
Diagram:
  parent: [VMA1] → anon_vma P
                    rb_root: [avc: VMA1 → P]
                             [avc: child_VMA1 → P]   ← added at fork
  
  child:  [child_VMA1] → anon_vma C
                          rb_root: [avc: child_VMA1 → C]
  
  Page X created BEFORE fork:
    page->mapping = P | 0x1
    
  Walk P's rb_root: find VMA1 (parent) and child_VMA1 (child)
  → Both VMAs may have PTEs for page X
  → Can scan both for PTE pointing to X

  Page Y created AFTER fork by child:
    page->mapping = C | 0x1
    
  Walk C's rb_root: find only child_VMA1
  → Only child maps this page (as expected — created after fork)
```

---

## 4. try_to_unmap(): Rmap Walk

```c
/* mm/rmap.c */

/* Unmap a page from all PTEs */
bool try_to_unmap(struct folio *folio, enum ttu_flags flags)
{
    struct rmap_walk_control rwc = {
        .rmap_one       = try_to_unmap_one,   // per-PTE callback
        .arg            = (void *)flags,
        .done           = folio_not_mapped,
        .anon_lock      = folio_lock_anon_vma_read,
    };
    
    rmap_walk(folio, &rwc);
    return !folio_mapped(folio);  // return true if all PTEs unmapped
}

void rmap_walk(struct folio *folio, struct rmap_walk_control *rwc)
{
    if (folio_test_anon(folio))
        rmap_walk_anon(folio, rwc, false);
    else
        rmap_walk_file(folio, rwc, false);
}

void rmap_walk_anon(struct folio *folio, struct rmap_walk_control *rwc, ...)
{
    struct anon_vma *anon_vma = folio_get_anon_vma(folio);
    // folio->mapping → anon_vma (clear bit 0)
    
    anon_vma_lock_read(anon_vma);
    
    // Walk the anon_vma's rb_root for all anon_vma_chain entries:
    anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root,
                                   folio->index, folio_end_pgoff(folio)) {
        struct vm_area_struct *vma = avc->vma;
        unsigned long address = vma_address(&folio->page, vma);
        
        if (rwc->invalid_vma && rwc->invalid_vma(vma, rwc->arg))
            continue;
        
        // Call the actual unmap function for this VMA:
        if (!rwc->rmap_one(folio, vma, address, rwc->arg))
            break;  // done or error
    }
    
    anon_vma_unlock_read(anon_vma);
    put_anon_vma(anon_vma);
}

bool try_to_unmap_one(struct folio *folio, struct vm_area_struct *vma,
                       unsigned long address, void *arg)
{
    // Find PTE in this VMA:
    pte_t *pte = page_check_address(page, vma->vm_mm, address, &ptl, 0);
    if (!pte) return true;  // PTE not found (already unmapped, race)
    
    // Clear PTE + flush TLB:
    ptep_clear_flush(vma, address, pte);
    // ARM64: pte_clear_full() → clear PTE to 0
    //        flush_tlb_page(vma, address) → TLBI VAE1IS + DSB ISH
    
    // Update page accounting:
    page_remove_rmap(page, vma, false);
    // Decrements page->_mapcount
    
    // If dirty page being unmapped (anonymous): must save to swap entry in PTE
    if (PageDirty(page)) {
        swap_entry_t entry = get_swap_page(page);
        // Encode swap entry into the (now clear) PTE slot
        set_swp_pte(vma->vm_mm, address, pte, entry);
        // ARM64: PTE has special encoding: bit[0]=0 (invalid), but
        //   bits[X:Y] encode swap device + offset
    }
    
    pte_unmap_unlock(pte, ptl);
    return true;
}
```

---

## 5. page_referenced(): Access Checking via rmap

```
page_referenced(): checks if a page has been recently accessed
  Used by: page reclaim to decide if page is "hot" (recently used)
  
  Mechanism:
    Walk all PTEs pointing to this page (via rmap)
    Check AF bit (Access Flag) in each PTE
    If AF=1: page was accessed recently
    Clear AF=1 → AF=0 (reset for next reclaim cycle)
    Return: number of references found
  
  ARM64 Access Flag:
    Hardware sets AF=1 on first access (if FEAT_HAFDBS not used)
    Or: if AF=0 → hardware raises access flag fault (ESR.DFSC=0b001011)
       → kernel sets AF=1 in page fault handler → performance tracking
    
    pte_young(pte): tests AF bit (bit[10]) in PTE
    ptep_clear_flush_young(vma, addr, ptep):
      clear AF in PTE + TLBI to force CPU to set it again on next access

LRU aging mechanism:
  Active list: pages with recent access (page_referenced > 0)
  Inactive list: pages not accessed recently
  
  Page reclaim tries inactive list first:
    page_referenced() on inactive page:
      If reference found: page_activate() → move to active list
      If no reference: proceed with reclaim (unmap + swap/free)
  
  ARM64 advantage: Hardware Access Flag (AF) + hardware dirty bit (DBM)
    If FEAT_HAFDBS (Hardware Access/Dirty Bit Support):
      CPU sets AF and "DBM dirty" bit without software page faults
      page_referenced() just reads AF, no fault overhead
    Default ARM64: software access flag management via fault handler
```

---

## 6. Interview Questions & Answers

**Q1: Why is the anon_vma hierarchy needed after fork()? Why not just put a pointer to the parent's anon_vma in the child?**

The key problem is that anonymous pages can be **created at different times**: some pages were created by the parent BEFORE fork (and shared with child), while new pages are created INDEPENDENTLY by each process AFTER fork.

If the child just used the parent's anon_vma:
- Pages created by the parent BEFORE fork: child's PTEs need to be findable from parent's anon_vma → OK, already there
- Pages created by the CHILD after fork: `page->mapping` would still point to parent's anon_vma → child pages would be searched in parent's rmap tree → wrong! And when parent exits and anon_vma is freed, child's pages would lose their rmap pointer → crash.

The hierarchy (child gets own anon_vma PLUS a chain link to parent's) solves this:
- For pages created before fork: `page->mapping` = parent anon_vma, walk parent's rb_root → finds both parent VMA and child VMA (via the anon_vma_chain we added at fork time)
- For pages created after fork: `page->mapping` = child's own anon_vma, walk child's rb_root → only finds child VMA (correct!)

This also handles grandchild processes: each generation adds another level to the chain, ensuring pages can always be found from the anon_vma that was current when the page was created.

---

## 7. Quick Reference

| Operation | Rmap Direction | Data Structure Used |
|---|---|---|
| page fault | Forward (VA → PA) | Page tables (PGD→PTE) |
| page reclaim | Reverse (PA → all PTEs) | anon_vma rb_root or address_space i_mmap |
| page migration | Reverse (update all PTEs) | rmap_walk() |
| fork CoW | Reverse (find all mappings) | anon_vma_chain |

| Function | Purpose |
|---|---|
| anon_vma_prepare(vma) | Allocate anon_vma for first anon page in VMA |
| anon_vma_fork(new_vma, old_vma) | Set up anon_vma chains after fork |
| page_add_new_anon_rmap(page, vma, addr) | Register new anonymous page |
| page_remove_rmap(page, vma) | Unregister (unmap) anonymous page |
| try_to_unmap(folio, flags) | Remove page from all PTEs (reclaim) |
| page_referenced(folio, ...) | Count recent PTEs pointing to page (LRU) |
