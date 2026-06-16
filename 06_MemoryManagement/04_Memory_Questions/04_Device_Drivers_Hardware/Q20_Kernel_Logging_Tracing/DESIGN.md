# Q20 — Design a Kernel-Level Logging and Tracing System

---

## 1. Problem Statement

Kernel observability is critical for debugging performance bottlenecks, latency spikes, and correctness bugs in production systems. A kernel logging and tracing system must:
- Collect structured events at nanosecond resolution with minimal overhead.
- Support dynamic insertion of trace points without kernel rebuild.
- Provide a ring buffer that survives brief overloads without blocking the kernel path.
- Allow eBPF programs to attach to trace points and aggregate data in-kernel.
- Export data to user space with zero-copy where possible.

Design a complete kernel tracing system from trace point insertion through ring buffer management to userspace export.

---

## 2. Requirements

### 2.1 Functional Requirements
- Static trace points (TRACE_EVENT macros) with < 10 ns overhead when disabled.
- Dynamic function tracing via ftrace (function graph, call graph).
- eBPF kprobe/tracepoint programs: run BPF bytecode at any kernel location.
- Per-CPU ring buffers: prevent cross-CPU contention in trace write path.
- Ring buffer consumer: user-space reads via `/sys/kernel/tracing/trace_pipe`.
- perf_event integration: hardware PMU counters + software trace events.

### 2.2 Non-Functional Requirements
- Disabled trace overhead: ≤ 1 ns (single `likely(false)` branch).
- Enabled trace write throughput: ≥ 10M events/sec per CPU.
- Ring buffer memory: configurable, default 4MB per CPU.
- Live tracing: events readable without stopping trace collection.

---

## 3. Constraints & Assumptions

- Linux ftrace infrastructure (`kernel/trace/`).
- tracefs mounted at `/sys/kernel/tracing`.
- eBPF via BPF subsystem (`kernel/bpf/`).
- x86-64 with hardware PMU (Intel Architectural PMU v4+).
- Per-CPU design throughout to minimize synchronization.

---

## 4. Architecture Overview

```
  Trace Sources:
  ┌────────────┐ ┌─────────────┐ ┌────────────┐ ┌───────────┐
  │TRACE_EVENT │ │  kprobe /   │ │  ftrace    │ │  perf_event│
  │  macros    │ │  uprobe     │ │  function  │ │  PMU HW   │
  └─────┬──────┘ └──────┬──────┘ └─────┬──────┘ └─────┬─────┘
        │               │              │               │
        ▼               ▼              ▼               ▼
  ┌─────────────────────────────────────────────────────────┐
  │           trace_event_call infrastructure               │
  │  trace_buffer_lock_reserve() → write to per-CPU ring   │
  └───────────────────────┬─────────────────────────────────┘
                          │
                          ▼
  ┌──────────────────────────────────────────────────────────┐
  │       Per-CPU Ring Buffer (struct trace_buffer)          │
  │  [CPU 0: 4MB] [CPU 1: 4MB] ... [CPU N: 4MB]            │
  │  Writer: lock-free, timestamped entries                 │
  │  Reader: blocking read from trace_pipe                  │
  └───────────────────────┬──────────────────────────────────┘
                          │
        ┌─────────────────┼──────────────────┐
        ▼                 ▼                  ▼
  /sys/kernel/        eBPF maps          perf ring buffer
  tracing/trace_pipe  (BPF_MAP_TYPE_*)   (perf mmap)
  (cat / trace-cmd)   (bpftool / bcc)    (perf record)
```

---

## 5. Core Data Structures

### 5.1 Per-CPU Ring Buffer Page

```c
/* Each per-CPU ring buffer is composed of linked pages */
struct buffer_page {
    struct list_head  list;          /* linked list of pages */
    local_t           write;         /* write offset within page (atomic, local_t) */
    unsigned          read;          /* read offset */
    struct buffer_data_page *page;   /* actual data page */
};

struct buffer_data_page {
    u64           time_stamp;        /* timestamp of first event on this page */
    local_t       commit;            /* committed write pointer */
    unsigned char data[];            /* packed trace events */
};

struct trace_buffer {
    struct buffer_page  **pages;     /* array of per-CPU page sets */
    int                   cpus;
    unsigned long         size;      /* bytes per CPU */
    /* Overwrite mode: oldest entries overwritten when full */
    bool                  overwrite;
};
```

### 5.2 Trace Event Entry

```c
/* Generic trace entry header */
struct trace_entry {
    unsigned short  type;       /* event type ID (matches trace_event_call) */
    unsigned char   flags;      /* TRACE_FLAG_IRQS_OFF, TRACE_FLAG_HARDIRQ */
    unsigned char   preempt_count;
    int             pid;        /* current->pid */
};

/* Example: sched_switch trace event */
struct trace_event_raw_sched_switch {
    struct trace_entry  ent;
    char    prev_comm[TASK_COMM_LEN];
    pid_t   prev_pid;
    int     prev_prio;
    long    prev_state;
    char    next_comm[TASK_COMM_LEN];
    pid_t   next_pid;
    int     next_prio;
};
```

### 5.3 TRACE_EVENT Macro Expansion

```c
/* Kernel code declares: */
TRACE_EVENT(sched_switch,
    TP_PROTO(bool preempt, struct task_struct *prev, struct task_struct *next),
    TP_ARGS(preempt, prev, next),
    TP_STRUCT__entry(
        __array( char, prev_comm, TASK_COMM_LEN )
        __field( pid_t, prev_pid )
        __field( long, prev_state )
        __array( char, next_comm, TASK_COMM_LEN )
        __field( pid_t, next_pid )
    ),
    TP_fast_assign(
        memcpy(__entry->prev_comm, prev->comm, TASK_COMM_LEN);
        __entry->prev_pid   = prev->pid;
        __entry->prev_state = prev->state;
        memcpy(__entry->next_comm, next->comm, TASK_COMM_LEN);
        __entry->next_pid   = next->pid;
    ),
    TP_printk("prev=%s[%d] state=%ld next=%s[%d]",
              __entry->prev_comm, __entry->prev_pid, __entry->prev_state,
              __entry->next_comm, __entry->next_pid)
);

/* Expansion creates: */
/* 1. trace_sched_switch() inline function (call site) */
/* 2. trace_event_raw_sched_switch struct */
/* 3. Registration in __tracepoints section */
/* 4. Format file in tracefs */
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Disabled-Trace Fast Path

When tracing is disabled, the overhead must be near-zero:

```c
/* TRACE_EVENT generates this at call site: */
static inline void trace_sched_switch(bool preempt, ...)
{
    if (static_key_false(&__tracepoint_sched_switch.key))
        do_trace_sched_switch(preempt, prev, next);
}

/* static_key_false() uses jump_label patching: */
/* Disabled: CPU executes NOP (5 bytes on x86, 1 ns) */
/* Enabled: kernel rewrites the NOP to JMP using text_poke_bp() */
/* This avoids branch misprediction — the NOP is unconditionally not-taken */
```

**Jump labels** are the critical optimization. Without them, a `likely(false)` check would still cause branch predictor side effects. Jump labels modify the instruction stream itself.

### 6.2 Ring Buffer Write — Lock-Free Per-CPU

The ring buffer writer is designed to be re-entrant (interrupts can trace while process-context trace is writing):

```c
/*
 * Ring buffer write uses a "commit loop":
 * 1. Reserve space: local_add(event_size, &page->write)
 *    → atomic increment, returns old value = our slot start offset
 * 2. Write event data into reserved slot
 * 3. Commit: local_add(event_size, &page->commit)
 *
 * If interrupted between reserve and commit:
 *   The interrupt traces into the SAME per-CPU ring at a higher nesting level
 *   Nested writer uses its own slot (already reserved by its own local_add)
 *   Outer writer commits after interrupt returns
 *
 * Reader only sees data up to committed offset (not just written offset)
 * → Prevents reading partially-written events
 */
```

### 6.3 eBPF Trace Programs

BPF programs attach to trace events without modifying kernel source:

```c
/* Kernel side: BPF attachment */
/* User loads BPF program via bpf(BPF_PROG_LOAD, ...) */
/* Attaches to tracepoint: */
bpf_prog_array_add(&tracepoint->funcs, bpf_prog);

/* Execution: after trace_entry is filled, kernel calls: */
for each bpf_prog in tracepoint->funcs:
    bpf_prog_run(bpf_prog, trace_entry_ptr)
    /* BPF program can: */
    /* - Read trace entry fields */
    /* - Aggregate into BPF maps (histograms, counters) */
    /* - Call BPF helpers: bpf_ktime_get_ns(), bpf_get_current_pid_tgid() */
    /* - bpf_perf_event_output() to send sample to userspace ring */
```

**BPF map usage for aggregation:**
```c
/* In-kernel histogram (no per-sample userspace copy): */
BPF_MAP_TYPE_ARRAY histogram;
/* latency_ns → bucket_count */
/* Only final histogram is read by userspace — massively reduces data transfer */
```

### 6.4 ftrace Function Tracing — Mcount

ftrace patches function prologues at kernel compile time:

```asm
/* Every kernel function, when compiled with -pg -mrecord-mcount: */
my_function:
    call __fentry__      ; patched to NOP when ftrace disabled
    push rbp
    mov rbp, rsp
    ...
```

When enabled:
```
__fentry__ → ftrace_ops_list_func() →
    for each ftrace_ops registered:
        call ops->func(ip, parent_ip, ops, regs)
        /* function tracer, function_graph_tracer, or BPF prog */
```

**Function graph tracer** records entry AND exit timestamp:
```c
/* Entry: records function_graph_entry_task_t on shadow stack */
/* Exit: reads entry, computes duration, records call graph */
/* Output: indented function call tree with duration per level */
```

### 6.5 perf_event — Hardware PMU Integration

```c
/* User creates perf_event (hardware counter): */
struct perf_event_attr attr = {
    .type            = PERF_TYPE_HARDWARE,
    .config          = PERF_COUNT_HW_CACHE_MISSES,
    .sample_freq     = 1000,      /* sample 1000 times/sec */
    .sample_type     = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN,
    .mmap            = 1,         /* enable mmap ring buffer */
};
fd = perf_event_open(&attr, pid, cpu, -1, 0);

/* Kernel: programs PMU counter via x86 MSRs */
wrmsr(MSR_ARCH_PERFEVTSEL0, event_select);
wrmsr(MSR_ARCH_PMC0, -period);   /* fires interrupt after N cache misses */

/* PMI (Performance Monitoring Interrupt): */
/* → perf NMI handler captures IP, call chain */
/* → writes sample to per-task mmap ring buffer */
/* → application mmap-reads samples without syscall */
```

---

## 7. Trade-off Analysis

| Mechanism | Overhead | Data Rate | Use Case |
|---|---|---|---|
| TRACE_EVENT (disabled) | ~1 ns (NOP) | N/A | Always-on trace points, zero cost when off |
| TRACE_EVENT (enabled) | ~100–500 ns | 10M events/sec | Debugging, latency profiling |
| eBPF kprobe | ~500–1000 ns | 5M events/sec | Custom filtering, aggregation in-kernel |
| ftrace function | ~50–200 ns | 50M calls/sec | Call graph profiling |
| perf PMU | Hardware interrupt | Sampling (not per-event) | Statistical profiling, cache analysis |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| TRACE_EVENT macro | `include/linux/tracepoint.h` | `TRACE_EVENT()`, `static_key_false()` |
| Ring buffer | `kernel/trace/ring_buffer.c` | `ring_buffer_lock_reserve()`, `ring_buffer_unlock_commit()` |
| ftrace core | `kernel/trace/ftrace.c` | `ftrace_call()`, `register_ftrace_function()` |
| Jump labels | `kernel/jump_label.c` | `static_key_enable()`, `__jump_table` section |
| BPF tracepoint | `kernel/bpf/tracing.c` | `bpf_prog_run_array()`, `trace_call_bpf()` |
| perf_event core | `kernel/events/core.c` | `perf_event_open()`, `perf_output_sample()` |
| kprobe | `kernel/kprobes.c` | `register_kprobe()`, `kprobe_ftrace_handler()` |
| tracefs | `kernel/trace/trace.c` | `trace_pipe_read()`, `tracing_open()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Ring Buffer Overflow (Events Lost)
```bash
cat /sys/kernel/tracing/per_cpu/cpu0/stats | grep "overrun"
# Increase buffer: echo 16384 > /sys/kernel/tracing/buffer_size_kb
# Or: use snapshot buffer for post-mortem analysis
echo 1 > /sys/kernel/tracing/snapshot   # capture current trace state
```

### 9.2 BPF Program Rejected by Verifier
```bash
# bpf() returns -EINVAL, dmesg shows verifier log
# Common causes: unbounded loop, missing NULL check, stack overflow
# Debug: bpftool prog load --debug prog.o /sys/fs/bpf/prog
```

### 9.3 ftrace Overhead Too High
```bash
# Function tracer traces ALL functions → huge overhead
# Use function_filter to narrow scope:
echo my_driver_function > /sys/kernel/tracing/set_ftrace_filter
echo function > /sys/kernel/tracing/current_tracer
```

---

## 10. Performance Considerations

- **Per-CPU ring buffers** eliminate all synchronization in the write path — writers never contend.
- **Jump labels** make disabled trace overhead unconditional NOP — not a branch, not a prediction miss.
- **BPF map aggregation** reduces user-space data transfer: aggregate 1M events/sec in-kernel into a single histogram read once per second.
- **perf mmap ring:** `perf_event_mmap_page` mapped into user space — samples readable without read() syscall.
- **Lockless ring buffer:** `local_t` counters (not atomic_t) used in per-CPU paths — x86 native store, no LOCK prefix.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Jump labels: disabled TRACE_EVENT = 5-byte NOP, not a conditional branch — no predictor cost.
2. Per-CPU ring buffer: `local_t` not `atomic_t` — no lock prefix needed for per-CPU counters.
3. Commit loop re-entrancy: nested writers (interrupt inside process context) both get their own slots.
4. eBPF map aggregation: process 10M events/sec in-kernel, ship only aggregated result to userspace.
5. ftrace mcount patching: compiler inserts `call __fentry__` → kernel patches to NOP at boot.
6. perf PMI: hardware counter overflow triggers NMI → sample IP + call chain → mmap ring.
7. `trace-cmd record / report`: tool of choice over raw tracefs for production use.
8. `bpftrace` one-liners: `bpftrace -e 'kprobe:vfs_read { printf("pid=%d\n", pid); }'`.
