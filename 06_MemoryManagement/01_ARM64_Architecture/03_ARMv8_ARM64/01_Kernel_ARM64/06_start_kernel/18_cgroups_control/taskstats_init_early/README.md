# `taskstats_init_early()` — Task Statistics Interface

## Purpose

Registers a netlink family for the `taskstats` interface, which provides per-task and per-process statistics to userspace via a Netlink socket. Used by tools like `iotop`, `cgacct`, and the `Delay Accounting` subsystem.

## Source File

`kernel/taskstats.c`

```c
static int __init taskstats_init_early(void)
{
    family_registered = (genl_register_family(&family) == 0);
    if (!family_registered)
        pr_err("TASKSTATS: early initialization failed\n");
    return 0;
}
early_initcall(taskstats_init_early);
```

## taskstats Structure

```c
struct taskstats {
    /* Version */
    __u16   version;
    __u32   ac_exitcode;    /* Exit status */
    
    /* Basic Accounting */
    __u8    ac_flag;        /* Record flags */
    __u8    ac_nice;        /* Task nice value */
    __u64   cpu_count;      /* Number of elapsed updates */
    __u64   cpu_delay_total; /* Delay waiting for CPU (ns) */
    
    /* Block I/O Delays */
    __u64   blkio_count;
    __u64   blkio_delay_total;  /* Total delay (ns) */
    
    /* Page fault delays */
    __u64   swapin_count;
    __u64   swapin_delay_total;
    
    /* CPU times */
    __u64   cpu_run_real_total;   /* CPU real time (ns) */
    __u64   cpu_run_virtual_total;/* CPU virtual time (ns) */
    
    /* Basic accounting (BSD process accounting compatible) */
    char    ac_comm[TS_COMM_LEN]; /* Command name */
    __u8    ac_sched;       /* Scheduling discipline */
    __u8    ac_pad[3];
    __u32   ac_uid;
    __u32   ac_gid;
    __u32   ac_pid;
    __u32   ac_ppid;
    /* ... many more fields ... */
};
```

## Netlink Interface

taskstats uses Generic Netlink (genl):

```c
// Userspace request (get stats for PID):
struct nlattr attrs[] = {
    { .nla_type = TASKSTATS_CMD_ATTR_PID, .value = pid }
};

// Kernel response: struct taskstats filled with current data
```

Tools like `iotop` use this to show per-process I/O statistics in real time.

## Delay Accounting

taskstats includes delay accounting fields that measure time spent waiting for:
- CPU (waiting in runqueue)
- Block I/O
- Page cache (waiting for page in)
- Swap in

See `delayacct_init()` for how these delays are measured.

## Cross-references

- [Phase overview](../README.md)
- `delayacct_init()`: [../delayacct_init/README.md](../delayacct_init/README.md)
