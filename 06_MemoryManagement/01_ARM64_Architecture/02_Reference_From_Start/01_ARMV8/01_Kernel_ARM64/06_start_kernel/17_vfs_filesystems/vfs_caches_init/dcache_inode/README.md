# Dentry and Inode Hash Tables

## Hash Table Overview

Both dentry and inode lookups use hash tables for O(1) average-case access.

### Dentry Hash Table

```
Lookup path: /usr/bin/gcc
    Component: "usr" in "/"
    hash = full_name_hash(parent_dentry, "usr", 3)
    bucket = hash & (dentry_hashtable_size - 1)
    Walk bucket linked list to find matching dentry
```

The hash table is allocated by `alloc_large_system_hash()` during `vfs_caches_init_early()`:

```c
dentry_hashtable = alloc_large_system_hash("Dentry cache",
        sizeof(struct hlist_bl_head),
        dhash_entries,   // Can be set with dhash_entries= cmdline
        13,              // 1/8192 of RAM by default
        HASH_EARLY | HASH_ZERO,
        &d_hash_shift,
        NULL,
        0,
        0);
```

### Inode Hash Table

Similar to dentry hash, but keyed on `(superblock, inode_number)`:

```c
inode_hashtable = alloc_large_system_hash("Inode-cache",
        sizeof(struct hlist_head),
        ihash_entries,
        14,              // 1/16384 of RAM
        HASH_EARLY | HASH_ZERO,
        &i_hash_shift,
        &i_hash_mask,
        0,
        0);
```

## LRU Reclaim

When memory pressure increases, the kernel reclaims unused dentries and inodes:

```
shrink_dcache_sb() / prune_dcache_sb()
    → Walk dentry_unused LRU list
    → For each unused dentry:
        → If reference count == 0:
            → Remove from hash table
            → Release inode reference
            → Free dentry object
```

The LRU is managed as a per-superblock list with a global shrinker interface:

```c
static struct shrinker dcache_shrinker = {
    .count_objects = dentry_cache_count,
    .scan_objects = dentry_cache_scan,
    .seeks = 1,
};
```

## Filesystem Integration

When a filesystem mounts, it registers a `super_operations` table that provides inode lifecycle methods:

```c
static const struct super_operations ext4_sops = {
    .alloc_inode    = ext4_alloc_inode,   // Alloc fs-specific inode
    .destroy_inode  = ext4_destroy_inode, // Free it
    .write_inode    = ext4_write_inode,   // Writeback dirty inode
    .evict_inode    = ext4_evict_inode,   // Last ref dropped
    .put_super      = ext4_put_super,
    // ...
};
```

## Cross-references

- [Parent: vfs_caches_init](../README.md)
