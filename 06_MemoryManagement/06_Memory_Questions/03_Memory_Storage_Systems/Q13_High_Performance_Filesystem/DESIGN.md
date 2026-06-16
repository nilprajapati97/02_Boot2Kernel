# Q13 — Design a High-Performance Filesystem (ext4/xfs-lite)

---

## 1. Problem Statement

Design a high-performance, journaling filesystem for a Linux block device. The filesystem must support:
- Crash consistency via journaling (or copy-on-write).
- High-throughput sequential I/O for ML training datasets.
- Low-latency metadata operations (stat, open, create) for millions of small files.
- Extent-based allocation to minimize fragmentation for large files.
- Scalable directory indexing for directories with millions of entries.

---

## 2. Requirements

### 2.1 Functional Requirements
- POSIX file semantics: create, read, write, stat, rename, link, unlink, mmap.
- Journaling (ordered mode) for crash consistency.
- Extent tree for file data blocks (not block map — avoids indirect block overhead).
- HTree (B+-tree) indexed directories for O(log N) lookup.
- Delayed allocation: batch block allocation at writeback time.
- Online resize (grow the filesystem while mounted).

### 2.2 Non-Functional Requirements
- Sequential read throughput: > 80% of raw device bandwidth.
- Metadata operations (stat): < 10 µs.
- Journal commit: < 5 ms for 128MB journal.
- Scalable to 64-bit block numbers (16 TB+ volumes).
- fsck recovery time: O(journal size), not O(filesystem size).

---

## 3. Constraints & Assumptions

- Block size: 4KB (matches page size for direct page cache integration).
- Journal: separate journal inode (like ext4's `.journal`).
- Journaling mode: **ordered** (data written before metadata committed).
- Allocation: best-fit with per-block-group freelists (like ext4 block groups).

---

## 4. Architecture Overview

```
  VFS Layer
  │  open()/read()/write()/stat() system calls
  ▼
  ┌────────────────────────────────────────────────────────────────┐
  │                VFS Inode + Dentry Cache                        │
  │  (dcache / icache: O(1) name lookup for hot entries)          │
  └────────────────────────────────────────────────────────────────┘
  │
  ▼
  ┌────────────────────────────────────────────────────────────────┐
  │              Filesystem Core (our_fs)                          │
  │                                                                │
  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────────────┐  │
  │  │  Inode Cache  │ │  Dir HTree   │ │  Extent Tree         │  │
  │  │  (hash table) │ │  (B+-tree)   │ │  (per-inode, 12+     │  │
  │  └──────────────┘ └──────────────┘ │  extent levels)       │  │
  │                                    └──────────────────────┘  │
  │  ┌──────────────────────────────────────────────────────────┐ │
  │  │           Block Allocator                                │ │
  │  │  Block Groups → Per-group bitmap → Delayed allocation   │ │
  │  └──────────────────────────────────────────────────────────┘ │
  │  ┌──────────────────────────────────────────────────────────┐ │
  │  │           Journal (JBD2)                                 │ │
  │  │  Transaction → Descriptor blocks → Commit block         │ │
  │  └──────────────────────────────────────────────────────────┘ │
  └────────────────────────────────────────────────────────────────┘
  │
  ▼
  ┌────────────────────────────────────────────────────────────────┐
  │              Block Device (NVMe / SSD / HDD)                   │
  └────────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Superblock (on-disk)

```c
struct our_fs_super_block {
    __le32  s_magic;            /* 0xOURFS */
    __le32  s_block_size;       /* bytes per block (4096) */
    __le64  s_blocks_count;     /* total blocks */
    __le64  s_free_blocks;      /* free blocks */
    __le64  s_inodes_count;     /* total inodes */
    __le64  s_free_inodes;      /* free inodes */
    __le32  s_blocks_per_group; /* blocks per block group */
    __le32  s_inodes_per_group;
    __le32  s_first_data_block; /* block number of first data block */
    __le64  s_journal_inum;     /* inode number of journal */
    __le32  s_feature_incompat; /* incompatible feature flags */
    __u8    s_uuid[16];         /* volume UUID */
    char    s_volume_name[16];
} __attribute__((packed));
```

### 5.2 Inode (on-disk)

```c
struct our_fs_inode {
    __le16  i_mode;             /* file type + permissions */
    __le16  i_uid, i_gid;
    __le64  i_size;             /* file size in bytes */
    __le32  i_atime, i_mtime, i_ctime;
    __le16  i_links_count;
    __le32  i_flags;            /* EXTENTS_FL, INLINE_DATA_FL, etc. */
    __le32  i_blocks_lo;        /* 512B blocks used (lo 32 bits) */

    /* Extent tree root OR inline data (60 bytes) */
    union {
        struct our_fs_extent_header eh_header; /* extent tree root */
        __u8 i_data[60];                       /* inline data for tiny files */
    } i_block;

    __le32  i_generation;       /* for NFS */
    __le32  i_file_acl;
    __le32  i_size_high;        /* upper 32 bits of i_size */
};
```

### 5.3 Extent Tree Node

```c
struct our_fs_extent_header {
    __le16  eh_magic;    /* 0xF30A */
    __le16  eh_entries;  /* number of valid entries */
    __le16  eh_max;      /* max entries in this node */
    __le16  eh_depth;    /* 0 = leaf, >0 = internal node */
    __le32  eh_generation;
};

struct our_fs_extent {          /* leaf node entry */
    __le32  ee_block;           /* first logical block covered */
    __le16  ee_len;             /* number of blocks (max 32768) */
    __le16  ee_start_hi;        /* physical block number (high 16 bits) */
    __le32  ee_start_lo;        /* physical block number (low 32 bits) */
};

struct our_fs_extent_idx {      /* internal node entry */
    __le32  ei_block;           /* first logical block covered */
    __le32  ei_leaf_lo;         /* physical block of child node */
    __le16  ei_leaf_hi;
    __le16  ei_unused;
};
```

**Why extents over block maps:**
- Classic indirect block map: 3 levels for files > 4MB → 3 I/Os just to find a block.
- Extent: one entry covers a contiguous run of 32768 blocks (128MB) — most files need 1-4 extents.
- Single I/O to the extent tree leaf covers gigabytes of a sequential file.

### 5.4 Directory Entry with HTree

```c
struct our_fs_dir_entry {
    __le32  inode;              /* inode number */
    __le16  rec_len;            /* length of this directory entry */
    __u8    name_len;           /* length of filename */
    __u8    file_type;          /* DT_REG, DT_DIR, DT_LNK, etc. */
    char    name[255];          /* filename (not null-terminated) */
};

/* HTree index (root of B+-tree in directory's first block): */
struct dx_root {
    struct our_fs_dir_entry dot;    /* "." entry */
    struct our_fs_dir_entry dotdot; /* ".." entry */
    struct dx_root_info {
        __le32  reserved_zero;
        __u8    hash_version;   /* HALF_MD4 or TEA */
        __u8    info_length;    /* = 8 */
        __u8    indirect_levels; /* depth of HTree */
        __u8    unused_flags;
    } info;
    struct dx_entry entries[];  /* hash → block mappings */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Journaling — JBD2 Ordered Mode

JBD2 (Journaling Block Device 2, shared with ext4) operates in three modes:
- **Writeback:** Metadata journaled; data written asynchronously (fastest, least safe).
- **Ordered:** Data written to disk before metadata committed (default — safe + fast).
- **Journal:** Data AND metadata journaled (safest, slowest).

**Transaction lifecycle:**
```
1. handle = jbd2_journal_start(journal, nblocks)
   → opens transaction, reserves journal space

2. jbd2_journal_get_write_access(handle, bh)
   → copies current buffer content to journal (before-image)

3. Modify buffer (file data / metadata)

4. jbd2_journal_dirty_metadata(handle, bh)
   → marks buffer as part of this transaction

5. jbd2_journal_stop(handle)
   → closes handle; transaction waits for all handles to stop

6. Commit thread (jbd2_journal_commit_transaction):
   a. Wait for all dirty data writes to complete (ordered: data before metadata)
   b. Write descriptor block (lists all journaled blocks)
   c. Write all modified metadata blocks to journal
   d. Write commit block (marks transaction complete)
   e. Checkpoint: write blocks from journal to their real locations
   f. Update journal superblock to free committed space
```

**Crash recovery:**
```
Mount → jbd2_journal_recover():
    scan journal for complete transactions (have commit block)
    for each complete transaction: replay metadata blocks to real locations
    stop at incomplete transaction (truncated write = crash point)
```

Recovery is O(journal size), not O(filesystem size) — this is the key advantage of journaling over fsck.

### 6.2 Delayed Allocation — Deferring Block Assignment

Traditional allocation: assign blocks when data is written (`write()`).
Delayed allocation: defer block assignment until writeback time.

**Benefits:**
1. Coalesce multiple small writes → one large contiguous allocation.
2. Reduce fragmentation: allocator sees full extent of needed blocks.
3. Writeback thread submits one large BIO instead of many small ones.

```c
/* write() marks folio dirty but sets DELAY_ALLOCATE flag — no block yet */
/* At writeback time: */
ext4_writepages():
    for each dirty folio range:
        blocks = ext4_map_blocks(inode, folio_range):
            /* Now allocate blocks: best-fit in same block group as inode */
            ext4_mb_new_blocks() → bitmap search → allocate contiguous run
            ext4_ext_insert_extent() → add extent to inode's extent tree
        submit_bio(READ/WRITE, blocks)
```

### 6.3 Block Allocator — Block Groups + Buddy System

```
Filesystem layout:
[Superblock][Group 0][Group 1]...[Group N]

Block Group layout:
[Group Descriptor][Block Bitmap][Inode Bitmap][Inode Table][Data Blocks...]

Block Group Descriptor:
  bg_block_bitmap: block number of this group's block bitmap
  bg_inode_bitmap: block number of this group's inode bitmap
  bg_inode_table:  block number of inode table start
  bg_free_blocks_count
  bg_free_inodes_count
```

Multi-block allocator (mballoc) for large allocations:
1. **Goal:** Find a contiguous run of N blocks.
2. **First try:** Same block group as the inode (spatial locality).
3. **Buddy system per group:** Track free runs as powers of 2 (like buddy allocator).
4. **Fallback:** Search other groups.

### 6.4 Directory HTree — Indexed Lookup

For a directory with 1 million entries:
- Linear scan: O(N) = 1M entries × 4B comparisons = slow.
- HTree (H for hash): B+-tree indexed by `hash(filename)`.

```
Lookup("myfile"):
    1. Hash filename: h = half_md4("myfile")
    2. Read HTree root block → find leaf block for hash h
    3. Read leaf block → linear scan (max ~100 entries per block)
    4. Return inode number
```

Total: 2-3 block reads for any filename in any size directory.

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Journaling mode | Ordered | Writeback / Journal | Ordered: good crash safety with near-writeback performance |
| Extent tree | Yes | Indirect block map | Extents: fewer I/Os, less fragmentation for large files |
| Delayed allocation | Yes | Immediate allocation | Delayed: larger contiguous allocations, less fragmentation |
| HTree directories | Yes | Linear scan | HTree: O(log N) for large directories |
| Block groups | Yes | Flat allocation | Groups: inode + data locality, reduce seek distance |
| Journal size | 128MB default | 1MB min, 1GB max | 128MB: allows long transactions, fast commit |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| ext4 superblock | `fs/ext4/ext4.h` | `struct ext4_super_block` |
| Extent tree | `fs/ext4/extents.c` | `ext4_ext_map_blocks()`, `ext4_ext_insert_extent()` |
| JBD2 journal | `fs/jbd2/journal.c` | `jbd2_journal_start()`, `jbd2_journal_commit_transaction()` |
| Block allocator | `fs/ext4/mballoc.c` | `ext4_mb_new_blocks()`, `ext4_mb_find_by_goal()` |
| HTree directory | `fs/ext4/namei.c` | `dx_probe()`, `htree_dirblock_to_tree()` |
| Delayed alloc | `fs/ext4/inode.c` | `ext4_writepages()`, `ext4_map_blocks()` |
| VFS inode ops | `fs/ext4/file.c` | `ext4_file_operations`, `ext4_dir_operations` |
| Crash recovery | `fs/jbd2/recovery.c` | `jbd2_journal_recover()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Journal Overflow
```bash
# "jbd2: Transaction too large (journal is 128MB)"
# Fix: increase journal size at mkfs time or use tune2fs:
tune2fs -J size=512 /dev/nvme0n1p1
```

### 9.2 Extent Tree Corruption
```bash
e2fsck -f /dev/nvme0n1p1   # comprehensive check + repair
# Or online check via debugfs:
debugfs /dev/nvme0n1p1 -R "dump_extents <inode_number>"
```

### 9.3 Performance Regression — Fragmentation
```bash
e2freefrag /dev/nvme0n1p1   # shows free space fragmentation
filefrag -v /path/to/file   # shows extent count per file
# Many extents per file = fragmented = use defrag or tune delayed alloc
e4defrag /path/to/file      # online defragmentation
```

### 9.4 Metadata Performance (Small Files)
```bash
# inode table on SSD: metadata I/O should be fast
# Check inode table distribution: dumpe2fs /dev/nvme0n1p1 | grep -i "inode table"
# Tune: mkfs.ext4 -T largefile4 (fewer inodes for large-file workloads)
#       mkfs.ext4 -T news (more inodes for many small files)
```

---

## 10. Performance Considerations

- **Block group local allocation:** Inode created in block group closest to parent directory. New file's data blocks allocated in same group as inode. Minimizes seek distance on rotational media; reduces PCIe DMA scatter on NVMe.
- **Extent preallocation:** For streaming writes, preallocate a large extent (`fallocate()`) to guarantee contiguous blocks before writing.
- **Journal checksum (jbd2 csum3):** CRC32c on each journal block — detects torn writes during crash recovery. Adds ~0.1% overhead.
- **mmap + page cache:** File data served from page cache — NVMe reads cached in DRAM. Sequential read at DRAM speed (~50 GB/s) after first pass.
- **`noatime` mount option:** Disabling access time updates eliminates a metadata write per read — important for read-heavy ML training datasets.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Extent tree vs block map — quantify the difference: 1 extent entry vs 3 indirect block lookups for a 1GB file.
2. JBD2 ordered mode crash consistency — data-before-metadata ordering guarantee.
3. Delayed allocation — how it enables large contiguous block runs and reduces fragmentation.
4. HTree for million-entry directories — `hash(name)` → B+-tree → O(log N).
5. Block group locality — inode + data blocks co-located to minimize I/O seek.
6. `e2fsck` vs journal recovery — journal is O(journal_size), fsck is O(filesystem_size).
7. `fallocate()` for preallocation — relevant for GPU checkpoint files (large, written sequentially).
