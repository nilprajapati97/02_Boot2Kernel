# Chapter 03 — Process Scheduling

> **Book:** Linux Kernel Development — Robert Love (3rd Edition)  
> **Goal:** Deeply understand how the Linux kernel decides which process/thread runs on the CPU and when, including CFS, real-time scheduling, and preemption.

---

## Learning Objectives
- Understand scheduling policies and priorities
- Master the CFS (Completely Fair Scheduler) algorithm
- Understand the run queue and red-black tree data structure
- Understand preemption in the kernel
- Understand real-time scheduling classes
- Understand load balancing across CPUs (SMP)

---

## Topic Index

| File | Description |
|------|-------------|
| [01_Scheduling_Policy_And_Priority.md](./01_Scheduling_Policy_And_Priority.md) | nice, priority, scheduling classes |
| [02_CFS_Completely_Fair_Scheduler.md](./02_CFS_Completely_Fair_Scheduler.md) | Virtual runtime, red-black tree, fairness |
| [03_Run_Queue_And_Red_Black_Tree.md](./03_Run_Queue_And_Red_Black_Tree.md) | struct rq, rb_tree, pick_next_task |
| [04_Scheduler_Entry_Points.md](./04_Scheduler_Entry_Points.md) | schedule(), __schedule(), context switch |
| [05_Preemption.md](./05_Preemption.md) | User preemption, kernel preemption, preempt_count |
| [06_Real_Time_Scheduling.md](./06_Real_Time_Scheduling.md) | SCHED_FIFO, SCHED_RR, RT scheduler |
| [07_Load_Balancing.md](./07_Load_Balancing.md) | SMP load balancing, migration |

---

## Chapter Flow

```mermaid
flowchart TD
    A[Scheduling Concepts\nneed, goals, history] --> B[Scheduling Classes\nFair vs RT vs Idle]
    B --> C[Priority System\nnice, prio, rt_priority]
    C --> D[CFS Algorithm\nvruntime + red-black tree]
    D --> E[Run Queue\nstruct rq per CPU]
    E --> F[schedule\(\)\nContext Switch]
    F --> G[Preemption\nwhen can we interrupt?]
    G --> H[Real-Time\nSCHED_FIFO / SCHED_RR]
    H --> I[SMP Load Balancing\nspread work across CPUs]
```
