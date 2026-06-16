# `proc_caches_init()` — Process-Related Slab Caches

## Purpose

Creates slab caches for the structures associated with every process: signal tables, file descriptor tables, filesystem info, virtual memory descriptors, and virtual memory structures.

## Source File

`kernel/fork.c`

```c
void __init proc_caches_init(void)
{
    unsigned int mm_size;
    
    // Signal handling structures:
    sighand_cachep = kmem_cache_create("sighand_cache",
            sizeof(struct sighand_struct), 0,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_TYPESAFE_BY_RCU |
            SLAB_ACCOUNT, sighand_ctor);
    
    signal_cachep = kmem_cache_create("signal_cache",
            sizeof(struct signal_struct), 0,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT, NULL);
    
    // File descriptor table:
    files_cachep = kmem_cache_create("files_cache",
            sizeof(struct files_struct), 0,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT, NULL);
    
    // Filesystem info:
    fs_cachep = kmem_cache_create("fs_cache",
            sizeof(struct fs_struct), 0,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT, NULL);
    
    // Virtual memory area:
    mm_size = sizeof(struct mm_struct);
    mm_cachep = kmem_cache_create_usercopy("mm_struct",
            mm_size, ARCH_MIN_MMSTRUCT_ALIGN,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT,
            offsetof(struct mm_struct, saved_auxv),
            sizeof_field(struct mm_struct, saved_auxv), NULL);
    
    vm_area_cachep = KMEM_CACHE(vm_area_struct,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT);
    
    mmap_init();
}
```

## Key Structures

### `struct signal_struct` — Shared by All Threads

```c
struct signal_struct {
    refcount_t          sigcnt;
    atomic_t            live;         // Live threads in group
    int                 nr_threads;
    
    struct list_head    thread_head;  // All threads in group
    
    // Process-wide limits:
    struct rlimit       rlim[RLIM_NLIMITS];
    
    // Job control:
    pid_t               pgrp;
    pid_t               session;
    
    // Process times:
    cputime_t           utime, stime, cutime, cstime;
    
    // Pending signals (shared):
    struct sigpending   shared_pending;
    
    // Exit:
    int                 group_exit_code;
    struct task_struct  *group_exit_task;
};
```

### `struct files_struct` — File Descriptor Table

```c
struct files_struct {
    atomic_t            count;         // Reference count
    
    struct fdtable      *fdt;          // Pointer to current FDT
    struct fdtable      fdtab;         // Embedded FDT (for small fd counts)
    
    spinlock_t          file_lock;
    unsigned int        next_fd;       // Next fd to try
    unsigned long       close_on_exec_init[1];  // Close on exec bitmap
    unsigned long       open_fds_init[1];       // Open fd bitmap
    struct file         *fd_array[NR_OPEN_DEFAULT]; // First 64 fds inline
};
```

### `struct mm_struct` — Virtual Memory Descriptor

```c
struct mm_struct {
    struct {
        // VMA tree (virtual memory areas):
        struct maple_tree   mm_mt;      // All VMAs in a maple tree
        
        // Code and data segments:
        unsigned long       mmap_base;  // base of mmap area
        unsigned long       task_size;  // size of user space
        unsigned long       start_code, end_code;
        unsigned long       start_data, end_data;
        unsigned long       start_brk,  brk;      // heap
        unsigned long       start_stack;           // stack
        
        // Page tables:
        pgd_t               *pgd;       // Page Global Directory
        
        // Counters:
        atomic_t            mm_users;   // Users (threads + mm_get)
        atomic_t            mm_count;   // References (threads + get_task_mm)
        
        // Locks:
        struct rw_semaphore mmap_lock;  // Protects VMA tree
    };
    // ...
};
```

## Cross-references

- [Phase overview](../README.md)
- `fork_init()`: [../fork_init/README.md](../fork_init/README.md) — task_struct slab
- `maple_tree_init()`: [../../07_data_structures/maple_tree_init/README.md](../../07_data_structures/maple_tree_init/README.md) — VMA tree
