# perf Performance Analysis Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), Linux performance profiling

---

## 1. Concept Foundation

perf is the standard Linux performance profiling tool.

Capabilities:
- CPU cycle counting
- cache miss profiling
- branch prediction tracking
- memory latency analysis
- call graph reconstruction

---

## 2. ARM64 Hardware Detail

### 2.1 PMU (Performance Monitoring Unit)

ARM64 cores have built-in PMU with programmable counters.
Events: cycles, instructions, cache misses, TLB misses, memory stalls.

### 2.2 Overflow-based sampling

perf samples when counter overflows (e.g., every 10,000 cycles).
Sampling captures context without recording every event.

---

## 3. Linux Kernel Implementation

### 3.1 perf core

perf_event_open() syscall provides access to PMU.
Per-task and system-wide profiling modes.

### 3.2 Ring buffer

perf maintains ring buffer for efficient event capture.
User-space reads buffer to extract profiling data.

### 3.3 Call graph unwind

DWARF or frame pointer based stack unwinding.
Builds call tree to identify hotspots.

---

## 4. Hardware-Software Interaction

perf workflow:
1. perf record -g workload (capture with call graph)
2. workload runs, PMU samples collected
3. perf report (analyze hotspots by function)
4. identify performance bottlenecks

---

## 5. Interview Q and A

Q1: How does perf differ from trace tools?
perf uses sampling for low overhead; trace captures all events (higher overhead).

Q2: What is the difference between perf stat and perf record?
stat: summary counts. record: detailed sample trace.

Q3: How do you identify cache miss bottlenecks with perf?
perf record -e LLC-load-misses followed by perf report.

Q4: Can perf profile system-wide or just single process?
Both; use -a for system-wide, -p for specific process.

Q5: What is IPC (instructions per cycle) and how does perf report it?
IPC measures execution efficiency; higher is better. perf stat shows IPC directly.

Q6: How do you collect branch prediction info?
perf record -b; enable branch sampling in perf record.

---

## 6. Pitfalls and Gotchas

- Sampling rate too low missing important events.
- Call graph overhead slowing profiled workload.
- Misinterpreting perf percentages (function time vs. total time).
- Forgetting that perf sampling affects cache behavior.
- Not correlating perf results with source code line numbers (requires debug symbols).

---

## 7. Quick Reference Table

| Command | Purpose |
|---|---|
| perf stat workload | summary performance counters |
| perf record -g workload | detailed profiling with call graph |
| perf report | analyze recorded data |

| Event | Interpretation |
|---|---|
| cycles | CPU clock cycles (lower is better) |
| cache-misses | L1/L2/LLC misses (lower is better) |
| TLB-load-misses | translation lookaside buffer misses |
| stalled-cycles | cycles waiting for memory |
