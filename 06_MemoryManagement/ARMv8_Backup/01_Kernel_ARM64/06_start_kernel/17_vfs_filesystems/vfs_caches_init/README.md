# `vfs_caches_init()` — VFS Slab Caches

## Purpose

Creates the full set of slab caches for VFS data structures: dentries, inodes, filenames, and mount points. Also initializes the dentry and inode hash tables for fast path lookup.

## Source File

`fs/dcache.c` (dentry part), `fs/inode.c` (inode part)

```c
void __init vfs_caches_init(void)
{
    names_cachep = kmem_cache_create_usercopy("names_cache", PATH_MAX, 0,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC, 0, PATH_MAX, NULL);
    
    dcache_init();      // dentry cache and hash table
    inode_init();       // inode cache and hash table
    files_init();       // file struct slab and hash table
    files_maxfiles_init();
    mnt_init();         // vfsmount slab, namespace init, rootfs
}
```

## Dentry Cache (dcache)

The dentry cache maps pathname components to inodes. It is the core of fast path lookup.

### `struct dentry`

```c
struct dentry {
    unsigned int        d_flags;
    seqcount_spinlock_t d_seq;
    struct hlist_bl_node d_hash;        // Hash table entry
    struct dentry       *d_parent;      // Parent directory
    struct qstr         d_name;         // Name (hash + string)
    struct inode        *d_inode;       // Associated inode (NULL = negative)
    unsigned char       d_iname[DNAME_INLINE_LEN]; // Inline name for short names
    
    struct lockref      d_lockref;      // d_lock + reference count
    const struct dentry_operations *d_op;
    struct super_block  *d_sb;          // Superblock
    
    struct list_head    d_lru;          // LRU list
    struct list_head    d_child;        // In parent's d_subdirs
    struct list_head    d_subdirs;      // Our children
    union {
        struct hlist_node d_alias;      // Inode alias list
        struct hlist_bl_node d_in_lookup_hash;
        struct rcu_head d_rcu;
    };
};
```

### Negative Dentries

When a path component doesn't exist, the kernel caches a "negative dentry" — a dentry with `d_inode = NULL`. This prevents repeated failed lookups from hitting the filesystem.

```bash
# Every time you type a non-existent command:
$ notacommand
# bash searches /usr/bin/notacommand → not found
# Kernel caches negative dentry for "notacommand"
# Second attempt: served from dentry cache (no disk I/O)
```

## Inode Cache

```c
struct inode {
    umode_t             i_mode;     // File type + permissions
    kuid_t              i_uid;
    kgid_t              i_gid;
    unsigned int        i_flags;
    
    const struct inode_operations *i_op;
    struct super_block  *i_sb;
    struct address_space *i_mapping; // Page cache for file data
    
    unsigned long       i_ino;       // Inode number
    loff_t              i_size;      // File size in bytes
    
    struct timespec64   i_atime;     // Access time
    struct timespec64   i_mtime;     // Modification time
    struct timespec64   i_ctime;     // Change time
    
    blkcnt_t            i_blocks;    // 512-byte blocks
    unsigned int        i_nlink;     // Hard link count
    
    void                *i_private;  // Filesystem-specific data
};
```

## Sub-topic: dcache and inode Details

See [dcache_inode/README.md](dcache_inode/README.md) for hash table sizing, LRU reclaim, and filesystem integration.

## Cross-references

- [Phase overview](../README.md)
- `vfs_caches_init_early()`: [../../04_memory_management/vfs_caches_init_early/README.md](../../04_memory_management/vfs_caches_init_early/README.md) — early init
