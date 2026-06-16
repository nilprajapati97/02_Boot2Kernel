# ftrace and Kernel Event Tracing Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), kernel event tracing

---

## 1. Concept Foundation

ftrace is the kernel's tracer infrastructure for capturing kernel events.

Capabilities:
- function call tracing
- event tracing (memory allocations, page faults, etc.)
- latency analysis
- performance profiling

---

## 2. ARM64 Hardware Detail

### 2.1 Tracer backend

ftrace uses ring buffer for efficiency.
Each CPU has independent buffer to minimize contention.

### 2.2 Timestamp precision

ARM64 supports high-resolution timers via CNTPCT (cycle counter).
Trace timestamps have nanosecond-level accuracy.

---

## 3. Linux Kernel Implementation

### 3.1 Tracer infrastructure

/sys/kernel/debug/tracing/ interface controls tracing.
Key files:
- available_tracers: list of available tracers
- current_tracer: active tracer
- events: list of available events
- set_event: enable/disable events
- trace: output ring buffer contents

### 3.2 Function tracing

function tracer: capture every function call (expensive but complete).
function_graph tracer: show call hierarchy with timing.

### 3.3 Event tracing

Specific events (e.g., mm:* for memory, sched:* for scheduling).
Filter events by criteria (task, range, etc.).

---

## 4. Hardware-Software Interaction

ftrace usage example:
1. identify slow memory operation
2. enable page fault and migration events: echo 1 > .../events/mm/mm_migrate_pages/enable
3. run workload
4. check trace: cat /sys/kernel/debug/tracing/trace
5. analyze: which migrations happened during slow period

---

## 5. Interview Q and A

Q1: Why use ftrace instead of perf for tracing?
ftrace is built-in and simpler for event tracing; perf is more powerful for sampling.

Q2: What is the overhead of function tracing?
Very high; can slow system by 50% or more. Use sparingly.

Q3: Can ftrace filter events?
Yes; set_event_filter allows conditional tracing (e.g., only for specific task).

Q4: How do you capture only memory-related events?
echo 'mm:*' > /sys/kernel/debug/tracing/set_event

Q5: What is latency_trace and how does it help?
Shows latency between tracer events; useful for finding bottlenecks.

Q6: How do you save trace output to file?
cat /sys/kernel/debug/tracing/trace > trace.log or use trace-cmd tool.

---

## 6. Pitfalls and Gotchas

- Enabling function tracing on entire system (performance nightmare).
- Forgetting to disable tracing after use (consumes CPU and memory).
- Buffer overrun losing early trace events.
- Misinterpreting timestamps (CPU clock dependent on frequency scaling).
- Not filtering events, capturing irrelevant noise.

---

## 7. Quick Reference Table

| File | Purpose |
|---|---|
| /sys/kernel/debug/tracing/available_tracers | list tracers |
| /sys/kernel/debug/tracing/set_event | enable/disable events |
| /sys/kernel/debug/tracing/trace | read captured events |
| /sys/kernel/debug/tracing/trace_options | tracer options |

| Event | Category |
|---|---|
| mm:mm_page_alloc | memory allocation |
| mm:mm_page_free | memory freeing |
| sched:sched_process_fork | process creation |
| page_fault | page fault event |
