# `proc_root_init()` — Mount /proc Filesystem

## Purpose

Registers the `procfs` filesystem type and mounts it as `/proc`, the kernel's primary interface for exposing runtime kernel state to userspace.

## Source File

`fs/proc/root.c`

```c
void __init proc_root_init(void)
{
    proc_init_kmemcache();
    set_proc_pid_nlink();
    proc_self_init();
    proc_thread_self_init();
    proc_symlink("mounts", NULL, "self/mounts");
    
    // Create standard /proc entries:
    proc_net_init();
    proc_mkdir("fs", NULL);
    proc_mkdir("driver", NULL);
    proc_mkdir("fs/nfsd", NULL);    // For NFS
    
#if defined(CONFIG_SUN_OPENPROMFS) || defined(CONFIG_SUN_OPENPROMFS_MODULE)
    proc_mkdir("openprom", NULL);
#endif
    proc_tty_init();
    proc_mkdir("bus", NULL);
    
    // Register procfs:
    proc_sys_init();  // /proc/sys
}
```

## /proc Tree Structure

```
/proc/
├── 1/              → init process (PID 1)
│   ├── cmdline     → process command line
│   ├── environ     → environment variables
│   ├── exe         → symlink to executable
│   ├── fd/         → open file descriptors
│   ├── maps        → virtual memory mappings
│   ├── mem         → process memory (readable)
│   ├── net/        → network info (namespace)
│   ├── ns/         → namespace bindings
│   ├── smaps       → detailed memory mappings
│   ├── stack       → kernel stack trace
│   ├── stat        → process statistics
│   ├── status      → human-readable status
│   └── wchan       → kernel function sleeping in
├── self/           → symlink to current process
├── thread-self/    → symlink to current thread
├── cpuinfo         → CPU features and speeds
├── meminfo         → memory statistics
├── mounts          → mounted filesystems
├── net/            → network statistics
│   ├── arp         → ARP table
│   ├── dev         → network interfaces
│   ├── tcp         → TCP connections
│   └── udp         → UDP sockets
├── sys/            → kernel tunables (sysctl)
│   ├── kernel/
│   │   ├── hostname
│   │   ├── pid_max
│   │   └── sysrq
│   ├── vm/
│   │   ├── dirty_ratio
│   │   └── swappiness
│   └── net/
└── sysrq-trigger   → write to trigger sysrq
```

## /proc/PID Entries

Each process directory is dynamically generated from `task_struct`:

```c
// fs/proc/base.c:
static const struct pid_entry tgid_base_stuff[] = {
    DIR("fd",         S_IRUSR|S_IXUSR, proc_fd_inode_operations, ...),
    DIR("fdinfo",     S_IRUSR|S_IXUSR, ...),
    DIR("ns",         S_IRUSR|S_IXUSR, ...),
    REG("cmdline",    S_IRUGO,  proc_cmdline_operations),
    REG("stat",       S_IRUGO,  proc_tgid_stat_operations),
    REG("statm",      S_IRUGO,  proc_statm_operations),
    REG("maps",       S_IRUGO,  proc_pid_maps_operations),
    REG("mem",        S_IRUSR|S_IWUSR, proc_mem_operations),
    // ... ~60 entries ...
};
```

## procfs is Not a Real Filesystem

`/proc` has no on-disk representation. All reads dynamically gather kernel data:

```c
// Reading /proc/meminfo calls:
static int meminfo_proc_show(struct seq_file *m, void *v)
{
    struct sysinfo i;
    si_meminfo(&i);       // ← reads actual kernel memory counters
    si_swapinfo(&i);
    
    seq_printf(m, "MemTotal:       %8lu kB\n", i.totalram << (PAGE_SHIFT-10));
    // ...
}
```

## Cross-references

- [Phase overview](../README.md)
- `seq_file_init()`: [../seq_file_init/README.md](../seq_file_init/README.md)
- `nsfs_init()`: [../nsfs_init/README.md](../nsfs_init/README.md)
