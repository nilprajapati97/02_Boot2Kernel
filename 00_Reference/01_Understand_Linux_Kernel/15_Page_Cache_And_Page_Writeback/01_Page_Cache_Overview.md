# 01 — Page Cache Overview

## 1. What is the Page Cache?

The **page cache** stores disk data in memory pages — the primary I/O cache in Linux.

- All file reads go through the page cache
- Writes are cached (write-back) and written to disk asynchronously
- `mmap()` file mappings are also backed by the page cache
- Swappable when memory is tight

---

## 2. Page Cache Benefits

```mermaid
graph TD
    A["read(fd, buf, 4096)"] --> B{Page in cache?}
    B -- Yes: Cache Hit --> C["copy_to_user() from cached page\n~100 ns"]
    B -- No: Cache Miss --> D["Schedule disk I/O\n~1-10 ms"]
    D --> E["Page loaded into cache"]
    E --> C
```

---

## 3. Cache Lookup

```c
/* Find page in page cache */
struct page *page = find_get_page(mapping, index);
/* index = file offset / PAGE_SIZE */

if (page) {
    /* Cache hit: page is pinned */
    /* ... use page data ... */
    put_page(page);
} else {
    /* Cache miss: allocate and read from disk */
    page = page_cache_alloc(mapping);
    add_to_page_cache_lru(page, mapping, index, GFP_KERNEL);
    /* Submit bio to fill the page */
}
```

---

## 4. Page Cache Eviction (LRU)

Pages are aged by two LRU lists:

```mermaid
stateDiagram-v2
    [*] --> Inactive: First access (add_to_page_cache_lru)
    Inactive --> Active: Second access in short time
    Active --> Inactive: No recent access (age scan)
    Inactive --> [*]: Memory pressure (try_to_free_pages)
    Active --> [*]: Heavy memory pressure
```

---

## 5. Cache Statistics

```bash
# Free shows cache size:
free -h
#       total    used    free   shared  buff/cache  available
# Mem:   16Gi    4.0Gi  1.0Gi   200Mi      11Gi       11Gi

# Detailed page cache info:
cat /proc/meminfo | grep -E 'Cached|Buffers|Active|Inactive'

# Drop page cache (for benchmarking):
echo 3 > /proc/sys/vm/drop_caches  # 1=pagecache, 2=dentries/inodes, 3=all
```

---

## 6. read() via Page Cache

```mermaid
sequenceDiagram
    participant App
    participant VFS
    participant PC as Page Cache
    participant FS as Filesystem
    participant Block as Block Layer

    App->>VFS: read(fd, buf, len)
    VFS->>PC: generic_file_read_iter()
    PC->>PC: find_get_page(mapping, index)
    alt Cache Hit
        PC-->>VFS: page data
    else Cache Miss
        PC->>FS: readpage(page) → ext4_readpage()
        FS->>Block: submit_bio()
        Block-->>FS: bio complete
        FS-->>PC: Page filled
        PC-->>VFS: page data
    end
    VFS->>App: copy_to_user()
```

---

## 7. Source Files

| File | Description |
|------|-------------|
| `mm/filemap.c` | Page cache core (find, add, readahead) |
| `mm/readahead.c` | Readahead algorithm |
| `mm/swap.c` | LRU list manipulation |
| `mm/vmscan.c` | Page reclaim/eviction |
| `include/linux/pagemap.h` | Page cache API |

---

## 8. Related Topics
- [02_address_space.md](./02_address_space.md) — address_space backing
- [03_Writeback_Mechanism.md](./03_Writeback_Mechanism.md) — Writing dirty pages back
