# Phase 17: VFS and Filesystem Initialization

## Overview

Initializes the Virtual Filesystem Switch (VFS) layer: full dentry/inode caches, page cache, signal infrastructure, `/proc` filesystem, and the namespace filesystem.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`vfs_caches_init()`](vfs_caches_init/README.md) | `fs/dcache.c` | Full dentry/inode slab caches |
| 2 | [`pagecache_init()`](pagecache_init/README.md) | `mm/filemap.c` | Page cache wait queues |
| 3 | [`signals_init()`](signals_init/README.md) | `kernel/signal.c` | Signal queue slab cache |
| 4 | [`seq_file_init()`](seq_file_init/README.md) | `fs/seq_file.c` | seq_file for /proc |
| 5 | [`proc_root_init()`](proc_root_init/README.md) | `fs/proc/root.c` | Mount /proc filesystem |
| 6 | [`nsfs_init()`](nsfs_init/README.md) | `fs/nsfs.c` | Namespace filesystem |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## VFS Architecture

```
User syscall (open, read, write, stat)
    ↓
VFS Layer (fs/namei.c, fs/vfs_iocb.c)
    ↓ (resolves path to dentry → inode)
Filesystem (ext4, tmpfs, xfs, btrfs)
    ↓
Block layer (blk-mq, schedulers)
    ↓
Device driver (SCSI, NVMe, virtio)
```

## Function Index

- [vfs_caches_init/](vfs_caches_init/README.md)
- [pagecache_init/](pagecache_init/README.md)
- [signals_init/](signals_init/README.md)
- [seq_file_init/](seq_file_init/README.md)
- [proc_root_init/](proc_root_init/README.md)
- [nsfs_init/](nsfs_init/README.md)
