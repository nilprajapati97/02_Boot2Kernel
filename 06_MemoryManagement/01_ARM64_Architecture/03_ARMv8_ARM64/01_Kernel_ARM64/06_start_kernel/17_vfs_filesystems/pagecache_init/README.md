# `pagecache_init()` — Page Cache Wait Queues

## Purpose

Initializes the page cache's wait queue hash table. Processes waiting for page I/O completion (read from disk) sleep on these wait queues and are woken up when the I/O completes.

## Source File

`mm/filemap.c`

```c
void __init pagecache_init(void)
{
    int i;
    
    for (i = 0; i < PAGE_WAIT_TABLE_SIZE; i++)
        init_waitqueue_head(&page_wait_table[i]);
    
    page_writeback_init();
}
```

## The Page Cache

The page cache stores recently read (and written) file data in memory. It is organized as a radix/XArray tree per `address_space` (one per file):

```
inode->i_mapping (struct address_space)
    ├── i_pages: XArray (page_index → struct page*)
    ├── host: back-pointer to inode
    ├── a_ops: address_space_operations
    │   ├── readpage / readahead
    │   ├── writepage / writepages
    │   └── direct_IO
    └── nr_pages: total cached pages
```

## Wait Queue Design

When a process reads a file:
1. `filemap_get_folio()` checks if the page is in cache
2. If not present: allocate a locked page, submit I/O, **sleep on wait queue**
3. When I/O completes, unlock the page
4. Sleeping processes wake up and retry

```c
// Wait for a page to become unlocked:
static inline void wait_on_page_locked(struct page *page)
{
    if (PageLocked(page)) {
        wait_queue_head_t *q = page_waitqueue(page);
        wait_event(*q, !PageLocked(page));
    }
}
```

## Hash Table Distribution

Using a hash table (not one queue per page) saves memory:

```c
// 256 wait queues, hashed by page address:
#define PAGE_WAIT_TABLE_BITS 8
#define PAGE_WAIT_TABLE_SIZE (1 << PAGE_WAIT_TABLE_BITS)

static wait_queue_head_t page_wait_table[PAGE_WAIT_TABLE_SIZE];

wait_queue_head_t *page_waitqueue(struct page *page)
{
    return &page_wait_table[hash_ptr(page, PAGE_WAIT_TABLE_BITS)];
}
```

## Writeback

`page_writeback_init()` sets up the writeback infrastructure:
- Per-backing-device writeback threads (`bdi-default` kthread)
- Dirty page ratio limits (`/proc/sys/vm/dirty_ratio`)
- Writeback timers for periodic flush

## Cross-references

- [Phase overview](../README.md)
- `mm_core_init()`: [../../04_memory_management/mm_core_init/README.md](../../04_memory_management/mm_core_init/README.md)
