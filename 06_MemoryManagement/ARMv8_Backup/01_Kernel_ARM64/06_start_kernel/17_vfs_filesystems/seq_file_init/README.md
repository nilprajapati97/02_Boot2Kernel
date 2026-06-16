# `seq_file_init()` — Sequential File Interface

## Purpose

Creates the slab cache for `struct seq_file`, a kernel abstraction that simplifies the creation of readable `/proc` entries. seq_file handles buffering, seek support, and iteration for large virtual files.

## Source File

`fs/seq_file.c`

```c
void __init seq_file_init(void)
{
    seq_file_cache = KMEM_CACHE(seq_file,
                                 SLAB_ACCOUNT | SLAB_PANIC);
}
```

## The Problem seq_file Solves

Early kernel `/proc` entries required manual buffer management:
```c
// Old way (error-prone):
static int read_proc(char *buf, char **start, off_t offset,
                     int count, int *eof, void *data) {
    // Had to handle partial reads, offsets, overflow...
    int len = sprintf(buf, "value: %d\n", myvalue);
    *eof = 1;
    return len;
}
```

seq_file provides a clean iterator interface.

## seq_file Interface

```c
// Filesystem driver implements these four operations:
static const struct seq_operations myproc_seq_ops = {
    .start = myproc_seq_start,  // Initialize iteration state
    .next  = myproc_seq_next,   // Advance to next element
    .stop  = myproc_seq_stop,   // Cleanup iteration state
    .show  = myproc_seq_show,   // Print one element
};

// seq_file handles:
// - Buffering (collects output, sends when buffer full)
// - Rewinding (handles lseek on /proc file)
// - Partial reads (caller reads in chunks)
```

## Example: `/proc/net/tcp`

```c
// net/ipv4/tcp_ipv4.c:
static int tcp4_seq_show(struct seq_file *seq, void *v)
{
    // v = current socket from iteration
    struct sock *sk = v;
    
    seq_printf(seq, "%4d: %08X:%04X %08X:%04X %02X ...\n",
               i, src_addr, src_port, dst_addr, dst_port, state);
    return 0;
}
```

## `struct seq_file`

```c
struct seq_file {
    char            *buf;        // Output buffer
    size_t          size;        // Buffer size
    size_t          from;        // Start offset in buf for next read
    size_t          count;       // Bytes in buf
    size_t          pad_until;
    loff_t          index;       // Iteration position
    loff_t          read_pos;    // Position in virtual file
    struct mutex    lock;
    const struct seq_operations *op;
    int             poll_event;
    const struct file *file;     // Back pointer to file
    void            *private;    // Iterator state
};
```

## Usage in Practice

Most `/proc` files use seq_file:
- `/proc/cpuinfo` — `arch/x86/kernel/cpu/proc.c`
- `/proc/meminfo` — `fs/proc/meminfo.c`
- `/proc/net/*` — various network subsystems
- `/proc/*/maps` — `fs/proc/task_mmu.c`
- `/proc/*/status` — `fs/proc/array.c`

## Cross-references

- [Phase overview](../README.md)
- `proc_root_init()`: [../proc_root_init/README.md](../proc_root_init/README.md) — mounts /proc
