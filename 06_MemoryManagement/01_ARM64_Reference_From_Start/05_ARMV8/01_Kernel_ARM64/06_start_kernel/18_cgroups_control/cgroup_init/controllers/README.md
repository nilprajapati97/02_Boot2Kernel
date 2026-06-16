# cgroup Controllers Reference

## cpu Controller

Controls CPU bandwidth allocation.

### cgroup v2 Interface

```
cpu.weight          — relative weight (1-10000, default 100)
cpu.max             — "quota period" in microseconds
                      e.g., "50000 100000" = 50% of one CPU
cpu.stat            — CPU usage statistics
cpu.pressure        — PSI (Pressure Stall Information)
```

### Bandwidth Limiting

The CFS bandwidth controller implements CPU quota:
- Each cgroup has a `cpu.cfs_quota_us` and `cpu.cfs_period_us`
- A "token bucket" refills `quota` microseconds every `period` microseconds
- If a cgroup exhausts its quota, all tasks are throttled until the next period

## memory Controller

Controls memory usage (anonymous, file, kernel).

```
memory.current      — current usage in bytes
memory.max          — hard limit (triggers OOM if exceeded)
memory.high         — soft limit (triggers reclaim)
memory.low          — protection threshold
memory.min          — guaranteed minimum (never reclaimed)
memory.swap.max     — swap limit
memory.stat         — detailed statistics
memory.pressure     — PSI memory pressure
memory.oom.group    — kill entire group on OOM
```

### OOM Behavior

When a cgroup hits `memory.max`:
1. The allocating task triggers the OOM killer
2. The OOM killer selects a victim within the cgroup
3. If `oom.group=1`, all tasks in the cgroup are killed

## io Controller

Controls block I/O bandwidth and IOPS.

```
io.max              — "MAJ:MIN rbps=N wbps=N riops=N wiops=N"
io.weight           — relative weight for blk-cgroup
io.stat             — I/O statistics
io.pressure         — PSI I/O pressure
```

## pids Controller

Limits the number of processes/threads.

```
pids.max            — maximum PID count (or "max" for unlimited)
pids.current        — current count
pids.events         — fork() rejections due to limit
```

## PSI — Pressure Stall Information

Modern kernels expose per-resource pressure metrics:

```bash
cat /sys/fs/cgroup/myapp/memory.pressure
# some avg10=0.00 avg60=0.00 avg300=0.00 total=0
# full avg10=0.00 avg60=0.00 avg300=0.00 total=0
```

- `some`: time (%) when at least one task is stalled
- `full`: time (%) when ALL tasks are stalled (no progress)

## Cross-references

- [Parent: cgroup_init](../README.md)
