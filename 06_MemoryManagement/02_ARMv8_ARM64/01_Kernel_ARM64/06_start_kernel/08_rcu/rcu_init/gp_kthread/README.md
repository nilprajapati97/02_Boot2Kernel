# Grace Period kthread — `rcu_gp_kthread`

## Purpose

The RCU grace period kthread is the background kernel thread responsible for managing RCU grace periods. It wakes up when a grace period is needed, coordinates with all CPUs to collect quiescent states, and then invokes callbacks.

## Source File

`kernel/rcu/tree.c`

## Thread Registration

```c
void __init rcu_init(void)
{
    /* ... setup ... */
    
    // Register kthread — actual thread creation happens when
    // kthread infrastructure is ready
    rcu_spawn_gp_kthread();
}

static int __init rcu_spawn_gp_kthread(void)
{
    t = kthread_create(rcu_gp_kthread, NULL, "rcu_sched");
    // Runs at SCHED_FIFO priority to ensure GP completes promptly
    sched_setscheduler_nocheck(t, SCHED_FIFO, &param);
    wake_up_process(t);
}
```

## kthread Main Loop

```c
static int rcu_gp_kthread(void *arg)
{
    for (;;) {
        // Wait for work
        wait_event_idle(rcu_gp_wq, READ_ONCE(rcu_state.gp_flags));
        
        // Start new grace period
        rcu_gp_init();
        
        // Wait for all CPUs to report quiescent states
        // (with periodic checks and forcing slow CPUs)
        rcu_gp_fqs_loop();
        
        // End grace period
        rcu_gp_cleanup();
        
        // Invoke callbacks whose GP is complete
        rcu_do_batch(&rdp->cblist);
    }
}
```

## Priority

The `rcu_gp` kthread runs at `SCHED_FIFO` priority 1 — above normal tasks but below RT tasks. This ensures grace periods complete promptly even under load, while not blocking hard real-time tasks.

## Stall Detection

If a CPU doesn't report a quiescent state within `rcu_cpu_stall_timeout` seconds (default 21s), RCU prints a CPU stall warning:

```
INFO: rcu_sched detected stalls on CPUs/tasks:
    3-...: (21000 ticks this GP) idle=001/0/0x2 softirq=0/0 fqs=5250
    (detected by 0, t=21001 jiffies, g=-1199, q=1234)
```

## Cross-references

- [rcu_init parent](../README.md)
- [Callback queuing](../callbacks/README.md)
