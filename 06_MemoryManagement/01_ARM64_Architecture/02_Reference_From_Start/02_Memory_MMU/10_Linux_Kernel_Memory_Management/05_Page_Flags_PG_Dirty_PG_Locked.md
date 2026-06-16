# Page Flags: PG_dirty, PG_locked, and the Page Flag System

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Page flags: per-physical-page state bits stored in struct page->flags
  Every physical page has one flags field (unsigned long = 64 bits on ARM64)
  These bits encode: page state, zone membership, NUMA node
  
  ARM64 flags layout (64-bit):
  
    [63:..] NUMA node bits (if CONFIG_NODES_SHIFT > 0)
    [..:..]  Zone index bits (ZONE_DMA=0, ZONE_NORMAL=1, ZONE_MOVABLE=2)
    [..:..]  Section bits (for SPARSEMEM memory model)
    [20:0]  Page state flags (PG_locked, PG_dirty, etc.)
  
  Zone bits: extracted by page_zonenum(page)
  NUMA node: extracted by page_to_nid(page)
  State flags: tested by PageXxx(page) macros

Two flag operation mechanisms:
  1. Atomic bit operations (set_bit, test_bit, clear_bit) — for flags that
     can be set/cleared concurrently (PG_locked, PG_dirty)
  2. Non-atomic (__set_bit, __clear_bit) — only when holding page lock
```

---

## 2. Important Page Flags Reference

```
Complete list of important page flags (include/linux/page-flags.h):

PG_locked (bit 0):
  Set:   lock_page() / trylock_page()
  Clear: unlock_page()
  Test:  PageLocked(page)
  Used:  During I/O (read/write to disk). Prevents two threads from
         simultaneously initiating I/O for the same page.
  ARM64: lock_page() uses atomic bit_spin_lock (uses WFE for efficiency)

PG_error (bit 1):
  Set:   SetPageError() when I/O error occurs
  Test:  PageError(page)
  Used:  Signals disk read failure; page content invalid

PG_referenced (bit 2):
  Set:   mark_page_accessed() on page access
  Clear: page reclaim (vmscan) when advancing LRU clock
  Test:  PageReferenced(page)
  Used:  LRU aging: two-chance algorithm

PG_uptodate (bit 3):
  Set:   SetPageUptodate() after successful read
  Clear: ClearPageUptodate() on truncation
  Test:  PageUptodate(page)
  Used:  Buffer/page cache: data is valid (disk data loaded)

PG_dirty (bit 4):
  Set:   set_page_dirty() / __set_page_dirty_nobuffers()
  Clear: clear_page_dirty_for_io() before writeback I/O
  Test:  PageDirty(page)
  Used:  Marks pages that have been written but not yet synced to disk
  Critical: if a page is dirty and must be reclaimed, it must be written
            to disk first (writeback) before freeing

PG_lru (bit 5):
  Set:   lru_cache_add() when page added to LRU
  Clear: del_page_from_lru_list() when removed
  Test:  PageLRU(page)
  Used:  Whether page is tracked by the LRU subsystem

PG_active (bit 6):
  Set:   mark_page_accessed() (second access)
  Clear: deactivate_page() / page_not_uptodate()
  Test:  PageActive(page)
  Used:  Active LRU list (frequently accessed pages)
  
PG_slab (bit 7):
  Set:   SLUB slab allocation
  Test:  PageSlab(page)
  Used:  Page belongs to SLUB allocator

PG_writeback (bit 8):
  Set:   set_page_writeback() before sending to disk
  Clear: end_page_writeback() when I/O completes
  Test:  PageWriteback(page)
  Used:  Page currently being written to backing store
         Wait for: wait_on_page_writeback(page)

PG_compound (bit 15):
  Set:   prep_compound_page() during huge page allocation
  Test:  PageCompound(page) — only valid for HEAD pages
  Used:  Page is part of a compound page (THP, hugetlb, compound slab)

PG_reserved (bit 14):
  Set:   At boot time for special pages (BIOS, device memory)
  Test:  PageReserved(page)
  Used:  Page cannot be freed or migrated

PG_unevictable (bit 18):
  Set:   mlock(), ramfs, tmpfs with mlock, etc.
  Clear: munlock()
  Test:  PageUnevictable(page)
  Used:  Page on unevictable LRU list, never reclaimed
  
PG_swapbacked (bit 10):
  Set:   Allocated as anonymous page (can go to swap)
  Test:  PageSwapBacked(page)
  Used:  Distinguishes anon pages (swap) from file pages (disk writeback)

PG_mlocked (bit 26):
  Set:   mlock() on the VMA
  Test:  PageMlocked(page)
  Used:  Page is in a locked VMA (VM_LOCKED flag)

PG_reclaim (bit 22):
  Set:   Page being reclaimed (page_reclaim started)
  Test:  PageReclaim(page)
```

---

## 3. PG_locked: Detailed Implementation

```
PG_locked is THE critical synchronization primitive for page I/O.

Lock state machine:
    [unlocked] → lock_page() → [locked]
    [locked]   → unlock_page() → [unlocked]

lock_page(page):
  Calls: __lock_page(page)
    → bit_spin_lock(PG_locked, &page->flags)
    → If bit is 0 (unlocked): set it atomically, return
    → If bit is 1 (locked): call io_schedule() to sleep
                            Add to wait queue (page->waiters)
  
  ARM64 implementation:
    Uses LDAXR/STLXR (load-acquire exclusive / store-release exclusive)
    If bit is already set: ARM64 uses WFE (Wait For Event) for efficient
    spinning before escalating to io_schedule()

trylock_page(page):
  Returns: true if lock acquired, false if already locked
  No sleeping — used in interrupt context or when sleeping is unacceptable

unlock_page(page):
  __unlock_page() → clear_bit(PG_locked) → wake_up_page_bit()
  Wake up all processes sleeping in lock_page() for this page
  
  ARM64: STLR to clear the bit (store-release, ensuring visibility)

Why is page lock needed?
  Multiple threads accessing same file may fault on the same page
  Without lock: two threads start readahead for same page → two I/Os → race
  With lock: first thread locks, does I/O, unlocks; second thread
             sees locked page, waits, then finds PG_uptodate set → no I/O

Page lock + PG_uptodate protocol:
  if (!PageUptodate(page)) {
      lock_page(page);
      if (!PageUptodate(page)) {  // double-check after lock
          do_io_to_fill_page();
          SetPageUptodate(page);
      }
      unlock_page(page);
  }
  // Now page has valid data
```

---

## 4. PG_dirty and Writeback

```
Dirty page lifecycle:
  1. Page read from file → placed in page cache (mapping->i_pages)
     → SetPageUptodate, ClearPageDirty
  
  2. Process writes to the page (mapped or write syscall):
     → Page fault (if mapped) or write() copies data
     → set_page_dirty(page):
        → __set_page_dirty_buffers() or __set_page_dirty_nobuffers()
        → SetPageDirty(page)
        → marks address_space as dirty (radix tree tag PAGECACHE_TAG_DIRTY)
        → may wake up pdflush/kjournald for writeback
  
  3. Memory pressure or sync:
     → wbc (writeback_control) selection: kernel picks dirty pages
     → lock_page(page): prevent I/O races
     → clear_page_dirty_for_io(page): clears PG_dirty (BEFORE writing!)
       Why before? To detect new dirtying during I/O (set again if written)
     → set_page_writeback(page): sets PG_writeback
     → page_mkclean(page): mark all PTEs clean (write-protect for write-tracking)
       ARM64: changes PTE AP bits to read-only
     → writeback: call a_ops->writepage() → starts disk I/O
  
  4. I/O completes:
     → end_page_writeback(page): clears PG_writeback
     → If page was re-dirtied during writeback: PG_dirty is set again
       → page must be written again in next writeback cycle

Dirty ratio control:
  /proc/sys/vm/dirty_ratio: % RAM that can be dirty before process blocks
  /proc/sys/vm/dirty_background_ratio: start background writeback
  ARM64 has no special dirty tracking hardware → all done in software
```

---

## 5. ARM64-Specific Bit Operations

```
ARM64 atomic bit operations (arch/arm64/include/asm/bitops.h):

set_bit(nr, addr):   LDXR/STXR loop OR atomic instruction
  If CONFIG_ARM64_LSE_ATOMICS:
    STADD  x0, [x1]   // LSE atomic OR (set bit)
  Else:
    1: LDXR  x0, [x1]
       ORR   x0, x0, #(1 << nr)
       STXR  w2, x0, [x1]
       CBNZ  w2, 1b

clear_bit(nr, addr): 
  If LSE: STADD x0, [x1]  // with negative mask (BIC)
  Else: LL/SC with AND

test_and_set_bit(nr, addr):
  Returns old value; sets bit atomically
  If LSE:
    LDSET  x0, x1, [x2]   // atomic load-ORR-store, returns old
  
  Critical for PG_locked: test_and_set_bit(PG_locked, &page->flags)

test_bit(nr, addr):
  LDR  x0, [x1]
  LSR  x0, x0, #nr
  AND  x0, x0, #1
  (non-atomic read — OK for testing since we don't need exclusivity)

Memory barriers with bit operations:
  set_bit: no implied barrier (use smp_mb__before_atomic if needed)
  test_and_set_bit: implies acquire/release for locking patterns
  In lock_page(): __lock_bit → LDAXR (acquire load) ensures ordering
  In unlock_page(): STLR (release store) ensures ordering
```

---

## 6. Interview Questions & Answers

**Q1: What is the difference between PG_dirty and PG_writeback? Can both be set at the same time?**

**PG_dirty**: set when the page has been **modified** but not yet written to its backing store. It means "this page needs writeback."

**PG_writeback**: set when the page is **currently being written** to disk (I/O is in progress). It means "writeback I/O is in flight."

**Can both be set simultaneously? YES, and it's an important case.**

The writeback sequence is:
1. `clear_page_dirty_for_io()` clears `PG_dirty`
2. `set_page_writeback()` sets `PG_writeback`
3. Disk I/O is initiated
4. During I/O: if the process writes to the page AGAIN → `set_page_dirty()` sets `PG_dirty` again
5. I/O completes: `end_page_writeback()` clears `PG_writeback`
6. The page still has `PG_dirty` set → scheduler will write it again

So `PG_dirty=1, PG_writeback=1` means: "page is currently being written to disk AND has already been re-modified during that write — it will need to be written again."

This subtle case is why `clear_page_dirty_for_io()` is called **before** initiating I/O, not after. If you cleared it after, you'd have a race: process modifies page after I/O starts but before you clear dirty → dirty flag lost → data not written to disk.

---

## 7. Quick Reference

| Flag | Set By | Clear By | Waiter |
|---|---|---|---|
| PG_locked | lock_page() | unlock_page() | lock_page() → io_schedule |
| PG_dirty | set_page_dirty() | clear_page_dirty_for_io() | writeback thread |
| PG_writeback | set_page_writeback() | end_page_writeback() | wait_on_page_writeback() |
| PG_uptodate | SetPageUptodate() | ClearPageUptodate() | lock_page + check |
| PG_active | mark_page_accessed() | deactivate_page() | vmscan |
| PG_lru | lru_cache_add() | del_page_from_lru() | vmscan |
| PG_referenced | mark_page_accessed() | page reclaim scan | vmscan |
| PG_unevictable | mlock() | munlock() | never reclaimed |

| ARM64 Bit Op | Function | Barrier |
|---|---|---|
| LDXR/STXR | test_and_set_bit | None (LL/SC) |
| LDAXR/STXR | bit_spin_lock (acquire) | Acquire |
| LDXR/STLXR | bit_spin_unlock (release) | Release |
| LDSET (LSE) | atomic set_bit | Configurable |
