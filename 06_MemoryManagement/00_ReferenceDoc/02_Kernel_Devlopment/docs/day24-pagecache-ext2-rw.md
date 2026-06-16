# Day 24 — Page Cache + ext2 Write Support

> **Goal**: Add a simple page cache keyed by `(inode, index)`, route all file I/O through it, and add ext2 write paths: allocate blocks from the block bitmap, allocate inodes from the inode bitmap, append directory entries, and `fsync` dirty pages back to disk.
>
> **Why today**: Without a page cache every read goes to virtio (slow + serialized). Without write we cannot create files or modify configs. **This day closes Phase 5 — persistent storage is real.**

---

## 1. Background

### 1.1 Page cache role
```
read()/write()  ─►  inode  ─►  address_space  ─►  page_cache_lookup(idx)
                                                    │
                                  miss ─► block_read() into a freshly allocated page
                                  hit  ─► copy_to/from_user from the page
```
A page is identified by `(inode_ptr, page_index_within_file)`. The cache is a hash table.

### 1.2 Write-back model
- `write()` makes pages **dirty** but does not hit disk.
- `fsync()`, `umount`, or memory pressure trigger writeback.
- For simplicity we writeback **synchronously on `fsync`** and **lazily on inode evict**.

### 1.3 ext2 allocation
- Block bitmap (one block per group): a `1` bit means used.
- Inode bitmap: same idea.
- Group descriptor `bg_free_blocks_count` / `bg_free_inodes_count` updated alongside.
- Superblock `s_free_blocks_count` / `s_free_inodes_count` updated globally.

---

## 2. Design

### 2.1 Files
```
mm/page_cache.c
mm/page_cache.h
fs/ext2/alloc.c            (block + inode bitmap)
fs/ext2/write.c            (writepage, append dir entry)
fs/sync.c                  (sys_fsync)
```

### 2.2 Page cache structures
```c
struct page_cache_entry {
    struct inode *inode;
    u64    index;            /* file offset / PAGE_SIZE */
    void  *page_va;          /* direct-mapped */
    u8     dirty;
    u8     refcnt;
    struct page_cache_entry *next;       /* bucket chain */
};

#define PC_BUCKETS 1024
static struct page_cache_entry *pc_table[PC_BUCKETS];
static spinlock_t pc_lock;
```

### 2.3 Address-space ops (per FS)
```c
struct address_space_ops {
    int (*readpage) (struct inode *, u64 index, void *page);
    int (*writepage)(struct inode *, u64 index, void *page);
};
```
ext2 fills these with `ext2_readpage` (uses `ext2_bmap` + `block_read`) and `ext2_writepage` (allocates block if needed, then `block_write`).

---

## 3. Implementation

### 3.1 Cache primitives
```c
static u32 pc_hash(struct inode *i, u64 idx) {
    return ((uintptr_t)i ^ (idx * 2654435761u)) % PC_BUCKETS;
}

struct page_cache_entry *pc_get(struct inode *i, u64 idx)
{
    u32 h = pc_hash(i, idx);
    spin_lock(&pc_lock);
    for (struct page_cache_entry *e = pc_table[h]; e; e = e->next)
        if (e->inode == i && e->index == idx) {
            e->refcnt++; spin_unlock(&pc_lock); return e;
        }
    /* miss — allocate */
    struct page_cache_entry *e = kzalloc(sizeof *e, 0);
    phys_addr_t pa = alloc_pages(0);
    e->inode = i; e->index = idx; e->refcnt = 1;
    e->page_va = (void*)(pa + 0xffff000000000000UL);
    e->next = pc_table[h]; pc_table[h] = e;
    spin_unlock(&pc_lock);

    i->i_sb->s_aops->readpage(i, idx, e->page_va);
    return e;
}

void pc_put(struct page_cache_entry *e) {
    spin_lock(&pc_lock);
    if (--e->refcnt == 0 && !e->dirty) {
        /* could evict immediately; we keep until pressure */
    }
    spin_unlock(&pc_lock);
}
```

### 3.2 Generic `read` / `write` through cache
```c
long generic_file_read(struct file *f, char __user *buf, size_t n, u64 *pos)
{
    struct inode *i = f->f_inode;
    if (*pos >= i->i_size) return 0;
    if (*pos + n > i->i_size) n = i->i_size - *pos;
    size_t copied = 0;
    while (copied < n) {
        u64 idx = (*pos) / PAGE_SIZE;
        u32 off = (*pos) % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - off;
        if (chunk > n - copied) chunk = n - copied;
        struct page_cache_entry *e = pc_get(i, idx);
        if (copy_to_user(buf + copied, (u8*)e->page_va + off, chunk)) { pc_put(e); return -14; }
        pc_put(e);
        *pos += chunk; copied += chunk;
    }
    return copied;
}

long generic_file_write(struct file *f, const char __user *buf, size_t n, u64 *pos)
{
    struct inode *i = f->f_inode;
    size_t copied = 0;
    while (copied < n) {
        u64 idx = (*pos) / PAGE_SIZE;
        u32 off = (*pos) % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - off;
        if (chunk > n - copied) chunk = n - copied;
        struct page_cache_entry *e = pc_get(i, idx);
        if (copy_from_user((u8*)e->page_va + off, buf + copied, chunk)) { pc_put(e); return -14; }
        e->dirty = 1;
        if (*pos + chunk > i->i_size) i->i_size = *pos + chunk;
        pc_put(e);
        *pos += chunk; copied += chunk;
    }
    /* mark inode dirty for later inode-table writeback */
    i->i_dirty = 1;
    return copied;
}
```

Replace `ext2_read`/`ext2_write` in file_operations with these two.

### 3.3 ext2 block allocator
```c
int ext2_alloc_block(struct super_block *sb, u32 *out_pb)
{
    struct ext2_sb_info *si = sb->s_fs_info;
    u8 *bm = kmalloc(si->block_size, 0);
    for (u32 g = 0; g < si->groups_count; g++) {
        if (si->bgdt[g].bg_free_blocks_count == 0) continue;
        ext2_read_block_n(si->bgdt[g].bg_block_bitmap, bm, si->block_size);
        for (u32 i = 0; i < si->block_size * 8; i++) {
            if (!(bm[i/8] & (1 << (i%8)))) {
                bm[i/8] |= (1 << (i%8));
                ext2_write_block_n(si->bgdt[g].bg_block_bitmap, bm, si->block_size);
                si->bgdt[g].bg_free_blocks_count--;
                ext2_sync_bgdt(sb, g);
                *out_pb = g * si->blocks_per_group + i + 1;
                kfree(bm); return 0;
            }
        }
    }
    kfree(bm); return -28;     /* ENOSPC */
}

int ext2_alloc_inode(struct super_block *sb, u32 *out_ino) { /* analogous */ }
```

### 3.4 `ext2_writepage` — assign block on demand
```c
int ext2_writepage(struct inode *i, u64 idx, void *page)
{
    struct ext2_inode_info *ii = i->i_private;
    struct ext2_sb_info    *si = i->i_sb->s_fs_info;
    u32 pb;
    if (idx < 12) {
        if (ii->i_block[idx] == 0) {
            if (ext2_alloc_block(i->i_sb, &pb) < 0) return -28;
            ii->i_block[idx] = pb;
            i->i_dirty = 1;
        }
        pb = ii->i_block[idx];
    } else {
        /* allocate indirect block if missing (omitted; analogous to direct) */
        return -22;
    }
    return block_write(pb * (si->block_size/512), si->block_size/512, page);
}
```

### 3.5 `sys_fsync` and writeback
```c
void writeback_inode(struct inode *i)
{
    spin_lock(&pc_lock);
    for (int b = 0; b < PC_BUCKETS; b++)
        for (struct page_cache_entry *e = pc_table[b]; e; e = e->next)
            if (e->inode == i && e->dirty) {
                i->i_sb->s_aops->writepage(i, e->index, e->page_va);
                e->dirty = 0;
            }
    spin_unlock(&pc_lock);
    if (i->i_dirty) {
        ext2_write_inode(i);     /* writes back i_block[] + size */
        i->i_dirty = 0;
    }
}

long sys_fsync(struct pt_regs *r)
{
    int fd = r->regs[0];
    struct file *f = current()->files->fd[fd];
    if (!f) return -9;
    writeback_inode(f->f_inode);
    return 0;
}
```

### 3.6 Directory create (append dir entry)
```c
int ext2_create(struct inode *dir, struct dentry *d, u16 mode)
{
    u32 ino;
    if (ext2_alloc_inode(dir->i_sb, &ino) < 0) return -28;
    struct inode *ni = ext2_iget(dir->i_sb, ino);
    ni->i_mode = mode | S_IFREG; ni->i_size = 0; ni->i_dirty = 1;
    /* zero out i_block[] */
    struct ext2_inode_info *ii = ni->i_private;
    memset(ii->i_block, 0, sizeof ii->i_block);
    /* Append dir entry into last block of dir */
    /* ... read last block, find tail, write new entry, adjust prev rec_len ... */
    writeback_inode(ni);
    d->d_inode = ni;
    return 0;
}
```

### 3.7 Boot-time init order
```c
mm_init();
page_cache_init();
virtio_mmio_scan();
register_filesystem(&ramfs_type);
register_filesystem(&ext2_type);
do_mount("ramfs", NULL);
cpio_populate(root_sb, initrd, initrd_size);
/* Optional pivot to ext2: */
do_mount("ext2", NULL);   /* mounted at /mnt or replace root */
```

---

## 4. Pitfalls

1. **Double-cache pages**: if you don't pin pages in `pc_get`, the page can evict between read and `copy_to_user`. Refcount strictly.
2. **Dirty bit lost on eviction**: never evict if `dirty != 0`. Writeback first.
3. **BGDT consistency**: when you flip a bitmap bit you MUST also decrement counts in BGDT and SB. mkfs's `fsck` will scream otherwise.
4. **Sparse blocks**: a read past EOF or into a hole returns zero — make sure `generic_file_read` clamps to `i_size`.
5. **Concurrency**: single big `pc_lock` is fine for now; document scalability TODO.
6. **Atomicity**: power-loss safety is **out of scope** — we have no journal. Document.

---

## 5. Verification (Phase 5 gate)

End-to-end demo:
```
$ cat > script.sh <<'EOF'
echo 'persistent' > /mnt/hello.txt
sync
poweroff
EOF
# reboot
$ cat /mnt/hello.txt
persistent
```

Without page cache: timing the same `cat /etc/passwd` 100× should drop from O(ms/block_read) to nanoseconds for cached pages.

GDB checks:
- `pc_table` non-empty after one read.
- A second read of the same offset increments `refcnt` without calling `block_read`.

---

## 6. Stretch

- LRU eviction with clock or active/inactive lists.
- `madvise(MADV_DONTNEED)` to drop pages.
- mmap of regular files: map page-cache pages directly into user VAs (with COW for `MAP_PRIVATE`).
- Background `flush` kthread waking every 5 seconds.

---

## 7. References

- *Understanding the Linux Kernel* (Bovet/Cesati) ch.15 — Page Cache.
- Linux `mm/filemap.c`, `fs/ext2/inode.c::ext2_get_block`.
- *Operating Systems: Three Easy Pieces* — FFS / ext2 chapter.
