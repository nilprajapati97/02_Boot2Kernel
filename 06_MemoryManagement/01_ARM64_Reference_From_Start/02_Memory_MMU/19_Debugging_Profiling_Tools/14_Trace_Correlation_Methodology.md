# Trace Correlation Methodology Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Single-tool debugging often misses root causes. Correlating traces across tools yields faster and more reliable diagnosis.

Typical sources:
- ftrace/tracepoints
- perf samples
- BPF event streams
- PSI/vmstat counters

---

## 2. ARM64 Hardware Detail

### 2.1 Clock and timestamp consistency

Cross-tool correlation requires stable timestamp domains.
On ARM64, timer sources can differ if not configured consistently.

### 2.2 CPU topology awareness

Per-CPU buffers and migrations can reorder perceived event timelines.

---

## 3. Linux Kernel Implementation

### 3.1 Correlation workflow

1. define incident window
2. capture ftrace events for causality
3. capture perf for hotspots
4. capture BPF counters for high-cardinality filtering
5. align by timestamp and PID/TID

### 3.2 Memory debugging case

Correlate:
- mm tracepoints (reclaim/migration)
- perf stall cycles
- PSI memory pressure trend

Outcome: identify causal chain, not just symptom.

---

## 4. Hardware-Software Interaction

Example:
1. latency spike reported
2. PSI memory.some rises
3. ftrace shows direct reclaim burst
4. perf shows stalled cycles in reclaim path
5. BPF shows affected cgroup/process set
6. root cause isolated to memory limit policy

---

## 5. Interview Q and A

Q1: Why is correlation better than isolated profiling?
It links symptoms to causes across subsystem boundaries.

Q2: What joins datasets reliably?
Timestamp + PID/TID + CPU + cgroup context.

Q3: Common failure mode in correlation?
Mismatched clocks or missing context fields.

Q4: How to keep overhead reasonable?
Narrow windows and selective events rather than full tracing.

Q5: What indicates reclaim-caused latency?
Temporal alignment of PSI spike, reclaim tracepoints, and stall-cycle growth.

Q6: How to productionize this?
Automated capture profiles triggered by SLO breach.

---

## 6. Pitfalls and Gotchas

- Capturing too much data and losing signal.
- Ignoring CPU migration effects on event order.
- Assuming correlation implies causation without control tests.
- Missing cgroup tags in multi-tenant systems.

---

## 7. Quick Reference Table

| Step | Description |
|---|---|
| Scope window | define exact incident interval |
| Multi-source capture | ftrace + perf + BPF + PSI |
| Normalize context | timestamps, PID/TID, CPU, cgroup |
| Build timeline | event sequence and dependency |
| Validate cause | reproduce with targeted control |
