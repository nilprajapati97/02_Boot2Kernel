# `vfs_caches_init_early()` — Early VFS Slab Setup

## Purpose

Allocates the first slab caches needed by the VFS: the `dentry_cache` and `inode_cache`. These caches are needed extremely early because the kernel uses the filesystem abstraction internally before any real filesystem is mounted.

## Source File

`fs/dcache.c`

```c
void __init vfs_caches_init_early(void)
{
    dcache_init_early();
    inode_init_early();
}
```

## What is a Dentry?

A **dentry** (directory entry) is the VFS object representing a pathname component:

```
/usr/bin/bash
 ^   ^   ^
 |   |   └── dentry for "bash"
 |   └────── dentry for "bin"
 └────────── dentry for "usr"
```

```c
struct dentry {
    unsigned int     d_flags;
    seqcount_spinlock_t d_seq;
    struct hlist_bl_node d_hash;    // Lookup hash table
    struct dentry   *d_parent;
    struct qstr      d_name;        // "bash", "bin", etc.
    struct inode    *d_inode;       // Points to file metadata
    /* ... many more fields ... */
};
```

## What is an Inode?

An **inode** is the VFS object representing a file/directory's metadata (independent of its name):

```c
struct inode {
    umode_t          i_mode;     // Permissions + type
    uid_t            i_uid;
    gid_t            i_gid;
    loff_t           i_size;
    struct timespec64 i_mtime;
    struct super_block *i_sb;   // Which filesystem
    const struct inode_operations *i_op;
    const struct file_operations  *i_fop;
    /* ... */
};
```

## Why "Early"?

The `dcache_init_early()` and `inode_init_early()` only set up the **hash tables** (using `alloc_large_system_hash()` with `memblock`), not the full slab caches. The full slab caches are created later in `vfs_caches_init()` (Phase 17) after the slab allocator is ready.

```
vfs_caches_init_early():   Hash tables only (memblock allocation)
vfs_caches_init():         Full slab caches (slab allocator)
```

## Hash Table Setup

```c
static void __init dcache_init_early(void)
{
    // dentry_hashtable: hashed by (parent dentry, name) 
    dentry_hashtable = alloc_large_system_hash("Dentry cache",
                          sizeof(struct hlist_bl_head),
                          dhash_entries, 13,
                          HASH_EARLY | HASH_ZERO,
                          &d_hash_shift, NULL, 0, 0);
}
```

## Pre-conditions

- `memblock` available (called before buddy allocator)

## Post-conditions

- `dentry_hashtable` allocated and zeroed
- `inode_hashtable` allocated and zeroed
- VFS can perform hash-based dentry/inode lookups

## Cross-references

- [Phase overview](../README.md)
- `vfs_caches_init()` (full): [../../17_vfs_filesystems/vfs_caches_init/README.md](../../17_vfs_filesystems/vfs_caches_init/README.md)
