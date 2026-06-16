# `profile_init()` — Legacy Kernel Profiling

## Purpose

Initializes the legacy kernel profiler (`readprofile`). This is a coarse-grained, timer-interrupt-based profiler that measures which kernel functions were executing when each timer tick fired.

## Source File

`kernel/profile.c`

```c
void __init profile_init(void)
{
    if (!prof_on)
        return;
    
    pr_info("Profiling shift: %d\n", prof_shift);
    
    // Allocate profile buffer:
    // One counter per (kernel_text_size >> prof_shift) chunk
    prof_len = (_etext - _stext) >> prof_shift;
    
    prof_buffer = kzalloc(prof_len * sizeof(atomic_t), GFP_KERNEL | __GFP_NOWARN);
    if (!prof_buffer)
        prof_buffer = alloc_pages_exact(prof_len * sizeof(atomic_t), 
                                         GFP_KERNEL | __GFP_NOWARN);
}
```

## How It Works

On each timer tick, if profiling is enabled, the profiler:
1. Reads the current `RIP` (instruction pointer) from the interrupted context
2. Computes `index = (RIP - _stext) >> prof_shift`
3. Atomically increments `prof_buffer[index]`

After running, you read `/proc/profile` to get raw counts, then use `readprofile` to map counts to symbols.

## Enabling

```bash
# Enable at boot:
profile=2   # prof_shift=2, 4-byte granularity

# Or via sysctl at runtime:
echo 1 > /proc/sys/kernel/profile

# Read results:
readprofile -m /boot/System.map > profile_results.txt
```

## Limitations vs `perf`

| Feature | `profile` | `perf` |
|---------|-----------|--------|
| Granularity | Jiffie (4ms) | PMU-based (configurable) |
| User-space | No | Yes |
| Call stacks | No | Yes |
| BPF | No | Yes |
| Modern | No | Yes |

The legacy profiler is largely superseded by `perf record -e cpu-clock -g`.

## Cross-references

- [Phase overview](../README.md)
- `perf_event_init()`: [../perf_event_init/README.md](../perf_event_init/README.md)
