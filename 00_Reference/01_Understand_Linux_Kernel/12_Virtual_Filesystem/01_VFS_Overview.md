# 01 — VFS Overview

## 1. What is the VFS?

The **Virtual Filesystem Switch (VFS)** is an object-oriented abstraction layer in the Linux kernel that:

- Provides a **uniform file API** (`open`, `read`, `write`, `close`, `stat`, `mmap`)
- Translates generic calls into **filesystem-specific operations**
- Allows mixing local, network, pseudo filesystems transparently

---

## 2. VFS Architecture

```mermaid
graph TD
    Syscall["System Call\nopen/read/write/close"]
    VFS["VFS Core\nfs/namei.c, fs/read_write.c"]
    FilesOps["struct file_operations\n(per-filesystem vtable)"]
    InodeOps["struct inode_operations\nnamei operations"]
    SuperOps["struct super_operations\n(mount/unmount)"]
    
    Syscall --> VFS
    VFS --> FilesOps & InodeOps & SuperOps
    FilesOps --> Ext4["ext4_file_operations"]
    InodeOps --> Ext4Inode["ext4_inode_operations"]
    SuperOps --> Ext4Super["ext4_sops"]
```

---

## 3. Key Data Structures Relationship

```mermaid
graph LR
    Process["task_struct"] --> FDT["files_struct\n(fd table)"]
    FDT --> File["struct file\n(open file)"]
    File --> Dentry["struct dentry\n(name → inode)"]
    Dentry --> Inode["struct inode\n(metadata)"]
    Inode --> SB["struct super_block\n(filesystem)"]
    SB --> FS["Filesystem Driver"]
```

---

## 4. VFS Object Operations (Vtables)

| Structure | Operations Struct | Purpose |
|-----------|------------------|---------|
| `super_block` | `super_operations` | Mount, unmount, sync, alloc_inode |
| `inode` | `inode_operations` | create, mkdir, lookup, rename, link |
| `file` | `file_operations` | read, write, llseek, mmap, ioctl |
| `dentry` | `dentry_operations` | hash, compare, revalidate |
| `address_space` | `address_space_operations` | readpage, writepage, direct_IO |

---

## 5. File Open Flow

```mermaid
sequenceDiagram
    participant U as User Process
    participant SC as Syscall Layer
    participant VFS as VFS
    participant DC as Dentry Cache
    participant FS as Filesystem (ext4)

    U->>SC: open("file.txt", O_RDONLY)
    SC->>VFS: do_sys_open()
    VFS->>VFS: getname() — copy path from user
    VFS->>VFS: do_filp_open()
    VFS->>DC: d_lookup() lookup dentry in cache
    DC-->>VFS: dentry (or miss)
    VFS->>FS: ext4_lookup() if cache miss
    FS-->>VFS: inode
    VFS->>VFS: Allocate struct file
    VFS->>VFS: f_op = inode->i_fop
    VFS-->>SC: fd (file descriptor)
    SC-->>U: int fd
```

---

## 6. Filesystem Registration

```c
/* Each filesystem registers itself */
static struct file_system_type ext4_fs_type = {
    .owner   = THIS_MODULE,
    .name    = "ext4",
    .mount   = ext4_mount,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

/* Registration at module init */
static int __init ext4_init_fs(void)
{
    return register_filesystem(&ext4_fs_type);
}
```

---

## 7. Source Files

| File | Description |
|------|-------------|
| `fs/namei.c` | Path resolution, lookup |
| `fs/open.c` | open/close system calls |
| `fs/read_write.c` | read/write dispatch |
| `fs/inode.c` | Inode cache |
| `fs/dcache.c` | Dentry cache |
| `fs/super.c` | Superblock management |
| `include/linux/fs.h` | All VFS structures |

---

## 8. Related Topics
- [02_Superblock.md](./02_Superblock.md)
- [03_Inode.md](./03_Inode.md)
- [04_Dentry.md](./04_Dentry.md)
- [05_File_Object.md](./05_File_Object.md)
