# Chapter 07 — Bottom Halves and Deferring Work

## Overview

Interrupt handlers (top halves) must be fast. Work that is too slow for an ISR is deferred to **bottom halves** — mechanisms that run the work later, outside of hard interrupt context.

```mermaid
flowchart TD
    IRQ["Hardware IRQ fires"] --> TOP["Top Half (ISR)\n• Acknowledge HW\n• Read status\n• Schedule bottom half\n• Return quickly"]
    TOP --> BH["Bottom Half\n(runs later, less restricted)"]

    BH --> S1["Softirq\n• Lowest overhead\n• Runs in soft IRQ context\n• Cannot sleep\n• Per-CPU, can run in parallel"]
    BH --> S2["Tasklet\n• Built on softirq\n• Serialized per-tasklet\n• Cannot sleep\n• Easier to use than softirq"]
    BH --> S3["Work Queues\n• Kernel threads\n• CAN sleep\n• Highest overhead\n• Most flexible"]
```

## Topics

| File | Topic |
|------|-------|
| [01_Why_Bottom_Halves.md](./01_Why_Bottom_Halves.md) | Design rationale, top vs bottom half |
| [02_Softirqs.md](./02_Softirqs.md) | Kernel softirq system, ksoftirqd |
| [03_Tasklets.md](./03_Tasklets.md) | Tasklets: simple deferred functions |
| [04_Work_Queues.md](./04_Work_Queues.md) | Work queues: process-context deferred work |
| [05_Choosing_A_Bottom_Half_Mechanism.md](./05_Choosing_A_Bottom_Half_Mechanism.md) | Decision guide and comparison |

## Key Files

```
include/linux/interrupt.h     — tasklet API
include/linux/workqueue.h     — work queue API
kernel/softirq.c              — softirq + tasklet + work implementation
kernel/workqueue.c            — work queue implementation
```
