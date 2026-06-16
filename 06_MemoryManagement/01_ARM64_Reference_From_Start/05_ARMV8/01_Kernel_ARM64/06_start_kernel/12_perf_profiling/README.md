# Phase 12: Performance Monitoring and Profiling

## Overview

Initializes the kernel's performance event infrastructure (`perf_events`) and the legacy profiling subsystem. After this phase, `perf stat`, `perf record`, and hardware performance counters are available.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`perf_event_init()`](perf_event_init/README.md) | `kernel/events/core.c` | perf events framework |
| 2 | [`profile_init()`](profile_init/README.md) | `kernel/profile.c` | Legacy kernel profiling |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## What is `perf`?

`perf` is the Linux performance analysis tool backed by the perf_events kernel infrastructure. It provides:

1. **Hardware Performance Counters (PMU)**: CPU cycles, cache misses, branch mispredictions
2. **Software Events**: Context switches, page faults, task migrations
3. **Tracepoints**: Pre-defined kernel trace events
4. **Probes**: Dynamic function tracing (kprobes, uprobes)
5. **BPF Programs**: Arbitrary analysis programs in eBPF

## Function Index

- [perf_event_init/](perf_event_init/README.md)
- [profile_init/](profile_init/README.md)
