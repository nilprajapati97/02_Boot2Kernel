# PSI (Pressure Stall Information) and Memory Pressure Signals Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), memory pressure metrics

---

## 1. Concept Foundation

PSI (Pressure Stall Information) provides real-time visibility into system memory pressure.

Metrics:
- cpu.some: some tasks are waiting
- memory.some: some tasks waiting for memory (reclaim latency)
- memory.full: all tasks blocked on memory
- io.some / io.full: similar for I/O pressure

---

## 2. ARM64 Hardware Detail

### 2.1 Sampling frequency

PSI tracks stalls at task context switches and through periodic timers.
Low overhead; suitable for continuous monitoring.

### 2.2 Definition of stall

Memory stall: task blocked on page reclaim or swap I/O.
Aggregated across system.

---

## 3. Linux Kernel Implementation

### 3.1 PSI tracking

task_struct->psi_flags tracks if task is in reclaim state.
Counters updated when task enters/exits stall.

### 3.2 Pressure calculation

Pressure = percentage of time where memory.some OR memory.full is true.
Exported via /proc/pressure/memory.

### 3.3 Interface

/proc/pressure/memory:
- some avg10=X.XX avg60=Y.YY avg300=Z.ZZ total=N
- full avg10=... avg60=... avg300=... total=N

avg10/60/300: 10s, 60s, 300s moving averages.

---

## 4. Hardware-Software Interaction

PSI usage:
1. monitoring application reads /proc/pressure/memory
2. high memory.some → reclaim latency affecting tasks
3. rising trend → approaching pressure threshold
4. enables proactive action: limit load, migrate workload, scale resources

---

## 5. Interview Q and A

Q1: What does memory.some > 50% indicate?
More than half the time, at least some tasks are stalled on memory operations.

Q2: Why is PSI better than direct memory metrics?
PSI directly measures impact on applications; memory metrics don't show latency effect.

Q3: How do you use PSI for workload control?
Monitor PSI thresholds; when exceeded, start rejecting new load or scale out.

Q4: Can PSI predict OOM?
Not directly, but sustained high memory.full pressure often precedes OOM.

Q5: What is difference between memory.some and memory.full?
some: some tasks blocked (others running). full: all tasks blocked (system thrashing).

Q6: How granular is PSI monitoring?
Samples every context switch; ~millisecond resolution typical.

---

## 6. Pitfalls and Gotchas

- Misinterpreting PSI averages (moving average, not instantaneous).
- Setting PSI alert thresholds too tight (triggers frequently on normal workload variance).
- Ignoring other metrics alongside PSI (context without full picture).
- Assuming PSI accounts for network/disk pressure (it doesn't; separate metrics).

---

## 7. Quick Reference Table

| Metric | Interpretation |
|---|---|
| memory.some < 10% | healthy memory pressure |
| memory.some 10-50% | moderate pressure, noticeable latency |
| memory.some > 50% | high pressure, significant reclaim overhead |
| memory.full > 0 | system thrashing; critical condition |

| Action | Trigger |
|---|---|
| proactive load rejection | memory.some sustained > 30% |
| scaling decision | memory.full sustained > 5% |
