# Q11 — Design a Page Cache System for Linux

---

## 1. Problem Statement

The page cache is the single largest consumer of kernel memory — it holds file data in RAM to avoid repeated disk I/O. Every `read()`, `mmap()`, and `write()` system call touches it. It must:
- Serve cached reads at DRAM speed (~100 GB/s) for hot data.
- Write-back dirty pages to storage asynchronously, never blocking the application.
- Evict cold pages under memory pressure without evicting hot working sets.
- Handle file-backed mmap regions where page faults are served from the cache.
- Integrate with the VFS layer so any filesystem can use it transparently.

---

## 2. Requirements

### 2.1 Functional Requirements
- Cache file data pages indexed by `(inode, page_offset)`.
- Serve `read()` from cache; fall back to filesystem `readpage()` on miss.
- Write-back dirty pages via `writeback` mechanism (pdflush / writeback threads).
- `mmap()` support: page faults satisfied from page cache.
- Invalidation: `truncate()`, `O_DIRECT`, and `sync()` must bypass/flush cache.
- Readahead: predict sequential access patterns and pre-fetch pages.

### 2.2 Non-Functional Requirements
- Cache lookup: O(1) via radix tree / XArray.
- Memory pressure response: evict Least Recently Used file pages within one reclaim cycle.
- Dirty page writeback: < 30 seconds from dirty time (controlled by `dirty_expire_centisecs`).
- Readahead window: dynamically grow up to 256 pages for sequential reads.

---

## 3. Constraints & Assumptions

- Linux 6.x: `struct folio` based, `XArray` backing store (replaced radix tree in 5.1).
- `struct address_space` per-inode owns the page cache for that file.
- `struct folio` is the unit of caching (replaces `struct page` for compound pages).
- Write-back via `bdi_writeback` kernel threads (one per backing device info).

---

## 4. Architecture Overview

```
  User Space read(fd, buf, len)
  │
  ▼
  VFS layer: vfs_read() → file->f_op->read_iter()
  │
  ▼
  Page Cache Lookup: find_get_folio(mapping, index)
  │                                │
  ├── HIT: copy to user buffer ────┘ ← direct from page cache
  │
  └── MISS:
       │
       ▼
       alloc_folio() + add_to_page_cache_lru()
       │
       ▼
       mapping->a_ops->readpage(file, folio)  ← filesystem read
       │        (e.g., ext4_readpage → submit_bio)
       ▼
       Storage I/O → DMA → folio data filled
       │
       ▼
       SetPageUptodate(folio)
       wake_up_folio(folio)    ← wakes waiting read()
       │
       ▼
       copy_folio_to_user(folio, buf)

  Write Path:
  write() → generic_perform_write()
           → find/alloc folio in cache
           → copy_from_user() into folio
           → mark_folio_dirty()
           → folio added to bdi_writeback dirty list
           → pdflush/writeback_thread flushes after dirty_expire_centisecs
```

---

## 5. Core Data Structures

### 5.1 Address Space (per-inode page cache root)

```c
struct address_space {
    struct inode           *host;           /* owner inode */
    struct xarray           i_pages;        /* XArray: page index → folio */
    struct rw_semaphore     invalidate_lock; /* protects truncate vs fault race */
    gfp_t                   gfp_mask;       /* allocation flags for new pages */
    atomic_t                i_mmap_writable;
    struct rb_root_cached   i_mmap;         /* tree of VMAs mapping this file */
    unsigned long           nrpages;        /* total cached pages */
    unsigned long           nrexceptional;  /* swap/hole entries */
    pgoff_t                 writeback_index; /* writeback cursor */
    const struct address_space_operations *a_ops;
    /* Writeback control */
    unsigned long           flags;          /* AS_EIO, AS_ENOSPC, etc. */
    struct writeback_control *wb_ctl;
};
```

### 5.2 XArray (replaces radix tree for page cache index)

```c
/*
 * XArray: a sparse array of void* indexed by unsigned long.
 * Stores: folio pointers (normal entries), swap entries, migration entries.
 * Thread-safe: internal xa_lock (spinlock) + lockless readers via RCU.
 */
struct xarray {
    spinlock_t  xa_lock;
    gfp_t       xa_flags;
    void __rcu *xa_head;   /* root: NULL, single entry, or pointer to xa_node */
};
```

### 5.3 Address Space Operations (filesystem callbacks)

```c
struct address_space_operations {
    int  (*readpage)(struct file *, struct folio *);
    int  (*writepage)(struct folio *, struct writeback_control *);
    int  (*writepages)(struct address_space *, struct writeback_control *);
    int  (*set_page_dirty)(struct folio *);
    int  (*readpages)(struct file *, struct address_space *,
                      struct list_head *, unsigned);
    /* Huge page variants */
    int  (*readahead)(struct readahead_control *);
    /* Direct I/O bypass */
    ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *);
    /* Migratability for NUMA balancing */
    int  (*migratepage)(struct address_space *, struct folio *new,
                        struct folio *old, enum migrate_mode);
    bool (*is_partially_uptodate)(struct folio *, size_t from, size_t count);
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Page Cache Lookup — O(1) via XArray

```c
struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{
    struct folio *folio;

    rcu_read_lock();
    folio = xa_load(&mapping->i_pages, index);  /* lockless XArray lookup */
    if (folio && folio_try_get_rcu(folio)) {     /* increment refcount */
        if (unlikely(folio_mapping(folio) != mapping)) {
            folio_put(folio);
            folio = NULL;
        }
    }
    rcu_read_unlock();
    return folio;
}
```

XArray lookup is O(1) average (radix-style tree with up to 6 levels for 64-bit index). Critical: done under RCU read lock — no spinlock for the hot read path.

### 6.2 Readahead — Sequential Access Detection

```c
struct file_ra_state {
    pgoff_t     start;          /* start of readahead window */
    unsigned    size;           /* size of readahead window (pages) */
    unsigned    async_size;     /* how far ahead to start async readahead */
    unsigned    ra_pages;       /* max readahead window (= bdi->ra_pages) */
    unsigned    mmap_miss;      /* mmap cache miss penalty counter */
    loff_t      prev_pos;       /* last read position (for sequential detection) */
};
```

Algorithm:
1. Detect sequential: `offset == prev_pos + prev_len`.
2. Double the window each sequential read: `ra->size = min(ra->size * 2, ra_pages)`.
3. Submit async readahead: `page_cache_async_readahead(mapping, ra, file, page, offset, req_size)`.
4. Reset on random access: `ra->size = ra->async_size = 0`.

### 6.3 Dirty Page Tracking and Write-Back

Dirty folios go through three tracking mechanisms:

**Per-folio:** `SetFolioDirty(folio)` sets `PG_dirty` flag.

**Per-mapping:** `mapping->flags` has `AS_DIRTY`; the mapping is added to `bdi_writeback.b_dirty` list.

**Per-BDI (Backing Device Info):** Each block device has a `bdi_writeback` with:
```c
struct bdi_writeback {
    struct backing_dev_info *bdi;
    struct list_head b_dirty;         /* dirty inodes */
    struct list_head b_io;            /* inodes being written back */
    struct list_head b_more_io;       /* inodes that need more writeback */
    unsigned long    work_list;       /* pending wb work items */
    struct task_struct *task;         /* writeback kthread */
};
```

Write-back trigger conditions:
- `dirty_background_ratio` exceeded → background writeback starts.
- `dirty_ratio` exceeded → direct reclaim blocks new writes (throttling).
- `dirty_expire_centisecs` (default 3000 = 30s) → age-based flush.
- `sync()` / `fsync()` → forced writeback of all dirty pages.

### 6.4 Page Cache Eviction — Two-List LRU

The reclaimer (`mm/vmscan.c`) uses four LRU lists per NUMA zone:
```
active_file:   recently accessed file-backed pages
inactive_file: candidates for eviction
active_anon:   recently accessed anonymous pages
inactive_anon: candidates for swap
```

Eviction preference order:
1. `inactive_file` (clean) → free immediately.
2. `inactive_file` (dirty) → write back, then free.
3. `inactive_anon` → swap out.

`mark_folio_accessed()` promotes a folio from inactive to active when accessed a second time (second-chance LRU).

### 6.5 File-Backed mmap — Fault Integration

```c
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
    struct file *file = vmf->vma->vm_file;
    struct address_space *mapping = file->f_mapping;
    pgoff_t offset = vmf->pgoff;

    folio = filemap_get_folio(mapping, offset);
    if (!folio) {
        /* Page not in cache: trigger readahead + readpage */
        folio = do_sync_mmap_readahead(vmf);
        if (!folio)
            return VM_FAULT_SIGBUS;
    }

    /* Install PTE mapping folio into VMA */
    vmf->page = folio_file_page(folio, offset);
    return VM_FAULT_LOCKED;  /* folio is locked, ready for PTE install */
}
```

### 6.6 O_DIRECT — Bypassing the Page Cache

```c
/* O_DIRECT read: DMA directly into user buffer */
generic_file_direct_write():
    filemap_write_and_wait_range()  /* flush dirty cache pages for range first */
    invalidate_inode_pages2_range() /* evict conflicting cache pages */
    mapping->a_ops->direct_IO()    /* submit BIO directly, bypassing cache */
```

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| XArray for page index | Yes (5.1+) | Radix tree | XArray has better locking granularity, cleaner API |
| Two-list LRU | Active + inactive | Single clock LRU | Resists streaming scan thrashing |
| Async writeback | bdi_writeback kthread | Synchronous in write path | Async: write() returns immediately, disk latency hidden |
| Readahead doubling | Exponential growth | Fixed window | Adapts to sequential workload depth |
| O_DIRECT invalidation | Flush then bypass | Just bypass | Avoids stale cached data after direct write |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| Page cache lookup | `mm/filemap.c` | `filemap_get_folio()`, `pagecache_get_page()` |
| Page cache add | `mm/filemap.c` | `add_to_page_cache_lru()` |
| Readahead | `mm/readahead.c` | `page_cache_async_readahead()`, `ondemand_readahead()` |
| Write-back | `mm/page-writeback.c` | `balance_dirty_pages()`, `wb_writeback()` |
| BDI writeback | `fs/fs-writeback.c` | `writeback_sb_inodes()`, `bdi_writeback_workfn()` |
| mmap fault | `mm/filemap.c` | `filemap_fault()` |
| LRU management | `mm/swap.c` | `folio_add_lru()`, `mark_folio_accessed()` |
| XArray | `lib/xarray.c` | `xa_load()`, `xa_store()`, `xas_find()` |
| Address space ops | `include/linux/fs.h` | `struct address_space_operations` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Page Cache Memory Leak (Inode Not Evicted)
```bash
cat /proc/meminfo | grep Cached      # file page cache size
cat /proc/slabinfo | grep dentry     # dentry/inode cache sizes
# Use fincore to check file's presence in cache:
fincore /path/to/file
```

### 9.2 Write-Back Stall (Dirty Throttling)
```bash
cat /proc/vmstat | grep dirty        # dirty pages, writeback pages
# If dirty pages > dirty_ratio * total_ram: writes stall
# Check: iostat -x 1 (high %util on storage device)
echo 40 > /proc/sys/vm/dirty_ratio  # temporary relief
```

### 9.3 Data Corruption — O_DIRECT vs Cached Mix
**Symptom:** Application writes via O_DIRECT, another reads via read() — gets stale data.
**Fix:** Always flush before O_DIRECT write via `fsync()` or use `O_SYNC`.

---

## 10. Performance Considerations

- **XArray lockless read:** `xa_load()` uses RCU for lockless reads — critical for concurrent file reads from many threads.
- **Folio order:** Readahead allocates large-order folios (e.g., 512KB) to reduce XArray overhead for large sequential reads.
- **bdi_max_ratio:** Limit dirty pages per device — prevents one slow device from consuming all dirty memory.
- **`vm.vfs_cache_pressure`:** Controls rate of inode/dentry cache reclaim vs page cache reclaim. Lower = more aggressive inode cache retention.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. XArray as the backing store — explain why it replaced radix tree (better locking, cleaner API).
2. Two-list LRU — active vs inactive, second-chance promotion, streaming workload resistance.
3. `address_space_operations.readpage` — the filesystem plug point for cache misses.
4. Writeback triggering conditions: ratio-based vs time-based vs sync-forced.
5. `filemap_fault()` as the bridge between page cache and VMA mmap.
6. O_DIRECT interaction — why you must flush/invalidate before bypassing cache.
7. Folio API — why Linux moved from `struct page` to `struct folio` for compound pages.
