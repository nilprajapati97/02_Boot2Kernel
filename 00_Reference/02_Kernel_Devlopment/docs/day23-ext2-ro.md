# Day 23 — ext2 Read-Only Driver

> **Goal**: Mount an ext2-formatted disk read-only. Read superblock + block group descriptor table, walk inodes through direct & indirect block pointers, parse directory entries, and plug into the VFS so `open("/etc/hostname", O_RDONLY)` works against the block device built on Day 22.
>
> **Why today**: ext2 is the simplest journal-less Unix FS with a stable spec, perfect for a teaching kernel. Day 24 layers a page cache and write support on top.

---

## 1. Background

### 1.1 On-disk layout (1 KiB blocks shown; ours use 4 KiB)
```
Block 0 (boot sector + padding)
Block 1: Superblock
Block 2..: Block Group Descriptor Table (BGDT)
Then for each group:
  block bitmap (1 block)
  inode bitmap (1 block)
  inode table  (N blocks)
  data blocks  (remaining)
```

### 1.2 Key superblock fields (`offset 1024..`)
| Off | Size | Field |
|---|---|---|
| 0  | 4 | inodes_count |
| 4  | 4 | blocks_count |
| 24 | 4 | log_block_size (`block_size = 1024 << log_block_size`) |
| 32 | 4 | blocks_per_group |
| 40 | 4 | inodes_per_group |
| 56 | 2 | magic = 0xEF53 |
| 88 | 2 | inode_size (≥128) |

### 1.3 Inode (typical 128 B)
| Off | Size | Field |
|---|---|---|
| 0  | 2 | mode (`S_IFREG`, perms) |
| 4  | 4 | size_lo |
| 26 | 2 | links |
| 40 | 60 | i_block[15]: 12 direct, 1 single-indirect, 1 double, 1 triple |
| 108 | 4 | size_hi (for files >4 GiB) |

### 1.4 Directory entry (variable length)
```c
struct ext2_dir_entry {
    u32 inode;
    u16 rec_len;       /* size of this entry, padded to 4 */
    u8  name_len;
    u8  file_type;
    char name[];       /* not NUL-terminated */
};
```

---

## 2. Design

### 2.1 Files
```
fs/ext2/ext2.h
fs/ext2/super.c
fs/ext2/inode.c
fs/ext2/dir.c
fs/ext2/file.c
```

### 2.2 In-memory state
```c
struct ext2_sb_info {
    u32  block_size;       /* in bytes */
    u32  inodes_per_group;
    u32  blocks_per_group;
    u32  inode_size;       /* 128 or 256 */
    u32  groups_count;
    struct ext2_group_desc *bgdt;   /* allocated */
};
```

### 2.3 Block I/O helper
```c
static int ext2_read_block(u32 blk, void *buf) {
    return block_read(blk * (BLOCK_SIZE/512), BLOCK_SIZE/512, buf);
}
```
Where `BLOCK_SIZE` from superblock.

---

## 3. Implementation

### 3.1 Mount
```c
int ext2_mount(struct file_system_type *t, const void *data, struct super_block **out)
{
    u8 *sb_buf = kmalloc(1024, 0);
    block_read(2, 2, sb_buf);                /* bytes 1024..2047 */
    struct ext2_super *es = (void*)sb_buf;
    if (es->magic != 0xEF53) return -22;

    struct super_block *sb = kzalloc(sizeof *sb, 0);
    struct ext2_sb_info *si = kzalloc(sizeof *si, 0);
    si->block_size = 1024 << es->log_block_size;
    si->inodes_per_group = es->inodes_per_group;
    si->blocks_per_group = es->blocks_per_group;
    si->inode_size = es->rev_level ? es->inode_size : 128;
    si->groups_count = (es->blocks_count + si->blocks_per_group - 1) / si->blocks_per_group;

    /* Load BGDT (starts at block right after SB block) */
    u32 bgdt_block = (si->block_size == 1024) ? 2 : 1;
    u32 bgdt_bytes = si->groups_count * sizeof(struct ext2_group_desc);
    u32 bgdt_blocks = (bgdt_bytes + si->block_size - 1) / si->block_size;
    si->bgdt = kmalloc(bgdt_blocks * si->block_size, 0);
    for (u32 i = 0; i < bgdt_blocks; i++)
        ext2_read_block_n(bgdt_block + i, (u8*)si->bgdt + i * si->block_size, si->block_size);

    sb->s_fs_info = si;
    sb->s_root_inode = ext2_iget(sb, 2);     /* root is always inode 2 */
    *out = sb;
    return 0;
}
```

### 3.2 `ext2_iget` — load inode #N
```c
struct inode *ext2_iget(struct super_block *sb, u32 ino)
{
    struct ext2_sb_info *si = sb->s_fs_info;
    u32 group = (ino - 1) / si->inodes_per_group;
    u32 idx   = (ino - 1) % si->inodes_per_group;
    u32 inode_table_block = si->bgdt[group].bg_inode_table;
    u32 byte_off = idx * si->inode_size;
    u32 block    = inode_table_block + byte_off / si->block_size;
    u32 in_block = byte_off % si->block_size;

    u8 *buf = kmalloc(si->block_size, 0);
    ext2_read_block_n(block, buf, si->block_size);
    struct ext2_inode *ei = (void*)(buf + in_block);

    struct inode *i = kzalloc(sizeof *i, 0);
    i->i_ino = ino; i->i_mode = ei->i_mode;
    i->i_size = ei->i_size | ((u64)ei->i_size_hi << 32);
    i->i_nlink = ei->i_links_count;
    i->i_sb = sb;
    i->i_op = &ext2_iops;
    i->i_fop = S_ISDIR(ei->i_mode) ? &ext2_dir_fops : &ext2_file_fops;

    /* Stash i_block[] in i_private */
    struct ext2_inode_info *ii = kzalloc(sizeof *ii, 0);
    memcpy(ii->i_block, ei->i_block, sizeof ii->i_block);
    i->i_private = ii;
    kfree(buf);
    return i;
}
```

### 3.3 Block mapping (logical → physical)
```c
static u32 ext2_bmap(struct inode *inode, u32 logical)
{
    struct ext2_inode_info *ii = inode->i_private;
    struct ext2_sb_info    *si = inode->i_sb->s_fs_info;
    u32 pbs = si->block_size / 4;           /* pointers per block */

    if (logical < 12) return ii->i_block[logical];

    logical -= 12;
    u32 buf[pbs];                           /* may be large; in real code allocate */
    if (logical < pbs) {
        ext2_read_block_n(ii->i_block[12], buf, si->block_size);
        return buf[logical];
    }
    logical -= pbs;
    if (logical < pbs * pbs) {
        ext2_read_block_n(ii->i_block[13], buf, si->block_size);
        u32 mid = buf[logical / pbs];
        ext2_read_block_n(mid, buf, si->block_size);
        return buf[logical % pbs];
    }
    /* triple-indirect — left as exercise */
    return 0;
}
```

### 3.4 File read
```c
static long ext2_read(struct file *f, char __user *buf, size_t n, u64 *pos)
{
    struct inode *i = f->f_inode;
    struct ext2_sb_info *si = i->i_sb->s_fs_info;
    if (*pos >= i->i_size) return 0;
    if (*pos + n > i->i_size) n = i->i_size - *pos;

    u8 *blk = kmalloc(si->block_size, 0);
    size_t copied = 0;
    while (copied < n) {
        u32 lb = (*pos) / si->block_size;
        u32 off = (*pos) % si->block_size;
        u32 chunk = si->block_size - off;
        if (chunk > n - copied) chunk = n - copied;
        u32 pb = ext2_bmap(i, lb);
        if (pb == 0) memset(blk, 0, si->block_size);   /* sparse */
        else        ext2_read_block_n(pb, blk, si->block_size);
        if (copy_to_user(buf + copied, blk + off, chunk)) { kfree(blk); return -14; }
        *pos += chunk; copied += chunk;
    }
    kfree(blk);
    return copied;
}
```

### 3.5 Directory iteration + lookup
```c
static struct dentry *ext2_lookup(struct inode *dir, const char *name)
{
    struct ext2_sb_info *si = dir->i_sb->s_fs_info;
    u8 *blk = kmalloc(si->block_size, 0);
    u32 nblocks = (dir->i_size + si->block_size - 1) / si->block_size;
    for (u32 lb = 0; lb < nblocks; lb++) {
        u32 pb = ext2_bmap(dir, lb);
        if (!pb) continue;
        ext2_read_block_n(pb, blk, si->block_size);
        u32 off = 0;
        while (off < si->block_size) {
            struct ext2_dir_entry *de = (void*)(blk + off);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len == strlen(name) &&
                !memcmp(de->name, name, de->name_len)) {
                struct dentry *d = kzalloc(sizeof *d, 0);
                memcpy(d->d_name, name, de->name_len);
                d->d_inode = ext2_iget(dir->i_sb, de->inode);
                kfree(blk); return d;
            }
            off += de->rec_len;
        }
    }
    kfree(blk);
    return NULL;
}
```

### 3.6 Wire into VFS
```c
struct file_system_type ext2_type = { .name = "ext2", .mount = ext2_mount };
register_filesystem(&ext2_type);
do_mount("ext2", NULL);     /* picks block device 0 */
```
Pivot root logic: once ext2 root is mounted, set `root_dentry` to ext2's root (or expose it under `/mnt`).

---

## 4. Building the disk image (host-side)

```
$ qemu-img create -f raw disk.img 64M
$ mkfs.ext2 -b 4096 disk.img
$ mkdir mnt && sudo mount -o loop disk.img mnt
$ sudo cp -r rootfs/* mnt/
$ sudo umount mnt
```

Or fully unprivileged with `genext2fs`:
```
$ genext2fs -b 16384 -B 4096 -d rootfs disk.img
```

---

## 5. Pitfalls

1. **Block size assumption**: don't hard-code 1024. Use `block_size << log_block_size` from SB.
2. **Inode size 256 (rev_level ≥1)**: include `i_size_hi`. We do.
3. **Endianness**: ext2 is little-endian; AArch64 LE → direct cast OK. Document.
4. **Sparse files**: a zero block pointer means "all zeros"; do not read disk block 0.
5. **Rec_len drift**: a corrupt `rec_len` of 0 must break the loop or you spin forever.
6. **Indirect block pointer table allocated on stack**: with 4 KiB blocks, that's 1024 u32 = 4 KiB on the kernel stack — borderline; kmalloc instead.

---

## 6. Verification

```c
struct file *f = filp_open("/etc/hostname", O_RDONLY);
char buf[64]; long n = vfs_read(f, buf, 63);
buf[n] = 0; printk("hostname: %s\n", buf);
```

```
[INFO] ext2: 16384 blocks, 4 KiB block size, 4 groups
[INFO] hostname: nkernel
```

GDB: check `ext2_sb_info.block_size == 4096`, `groups_count == 4`.

---

## 7. Stretch

- Read-only support for ext3 (just ignores the journal).
- `readdir` syscall (`getdents64`) so `ls` works.
- `stat` syscall returning POSIX `struct stat`.

---

## 8. References

- *The Second Extended File System: Internal Layout* (Dave Poirier).
- Linux `fs/ext2/*.c`.
- `mkfs.ext2(8)`, `genext2fs(8)`.
