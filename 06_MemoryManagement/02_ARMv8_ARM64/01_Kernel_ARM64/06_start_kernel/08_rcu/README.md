# Phase 8: RCU Initialization

## Overview

RCU (Read-Copy-Update) is Linux's most important synchronization primitive for read-heavy workloads. This phase initializes the full RCU machinery, including grace period tracking, callback queuing, and (optionally) the RCU/NO_HZ integration.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`rcu_init()`](rcu_init/README.md) | `kernel/rcu/tree.c` | Core RCU initialization |
| 2 | [`rcu_init_nohz()`](rcu_init_nohz/README.md) | `kernel/rcu/tree_plugin.h` | RCU + NO_HZ integration |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled

## What is RCU?

RCU is a synchronization mechanism optimized for frequent reads and infrequent writes:

```
Traditional rwlock:
  reader: must acquire read lock    (potential contention)
  writer: must wait for all readers (starvation risk)

RCU:
  reader: no lock needed! (rcu_read_lock = disable preemption)
  writer: modify a COPY, then publish new version
          wait for grace period (all readers finish)
          free old version
```

### Performance Comparison

| Operation | Spinlock | Rwlock | RCU |
|-----------|----------|--------|-----|
| Read (uncontended) | ~10ns | ~10ns | ~1ns |
| Read (contended) | blocking | blocking | non-blocking |
| Write | ~10ns | ~100ns+ | ~µs (grace period) |

## Key Concepts

- **Grace Period**: Time interval during which all pre-existing RCU read-side critical sections complete
- **Quiescent State**: A moment when a CPU is not in an RCU read-side critical section
- **Callback**: A function to call after the next grace period (to free old data)

## Function Index

- [rcu_init/](rcu_init/README.md)
- [rcu_init_nohz/](rcu_init_nohz/README.md)
