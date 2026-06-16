# `delayacct_init()` — Per-Task Delay Accounting

## Purpose

Initializes the per-task delay accounting slab cache. Delay accounting measures the time each task spends waiting for kernel resources, enabling diagnosis of I/O latency, CPU scheduler latency, and memory reclaim overhead.

## Source File

`kernel/delayacct.c`

```c
void __init delayacct_init(void)
{
    delayacct_cache = KMEM_CACHE(task_delay_info,
                                  SLAB_PANIC | SLAB_ACCOUNT);
    delayacct_tsk_init(&init_task);
}
```

## `struct task_delay_info`

```c
struct task_delay_info {
    spinlock_t  lock;
    
    /* Total delay in nanoseconds: */
    u64 blkio_start;          /* Time of last blkio start */
    u64 blkio_delay;          /* Waiting for block I/O */
    
    u64 swapin_start;
    u64 swapin_delay;         /* Waiting for page swap in */
    
    u64 freepages_start;
    u64 freepages_delay;      /* Waiting in page reclaim */
    
    u64 thrashing_start;
    u64 thrashing_delay;      /* Waiting for thrashing page */
    
    u64 compact_start;
    u64 compact_delay;        /* Waiting for memory compaction */
    
    /* Count of delay events: */
    u32 blkio_count;
    u32 swapin_count;
    u32 freepages_count;
    u32 thrashing_count;
    u32 compact_count;
};
```

## Measurement Points

The kernel instruments key wait points:

```c
// In mm/filemap.c (waiting for page I/O):
void io_schedule(void)
{
    delayacct_blkio_start();    // ← Start measuring
    schedule();
    delayacct_blkio_end(current); // ← End measuring
}

// In mm/vmscan.c (page reclaim):
static void shrink_page_list(...)
{
    delayacct_freepages_start();
    // ... reclaim pages ...
    delayacct_freepages_end();
}
```

## Enabling Delay Accounting

Delay accounting is opt-in per task (or globally via `CONFIG_TASK_DELAY_ACCT`):

```bash
# Enable for current process:
# Via taskstats netlink (tools like iotop do this automatically)

# Or compile with:
CONFIG_TASK_DELAY_ACCT=y
CONFIG_TASKSTATS=y
```

## Using Delay Accounting Data

```bash
# iotop uses delay accounting to show I/O wait per process:
iotop -o     # Show only processes doing I/O

# getdelays tool (from tools/accounting/):
getdelays -d -p $PID
# Shows:
# IO wait:    0.123s
# Swap in:    0.000s
# Reclaim:    0.001s
```

## Relationship to PSI

Pressure Stall Information (PSI) is the aggregate of individual task delays across all tasks, providing system-wide pressure metrics.

## Cross-references

- [Phase overview](../README.md)
- `taskstats_init_early()`: [../taskstats_init_early/README.md](../taskstats_init_early/README.md)
