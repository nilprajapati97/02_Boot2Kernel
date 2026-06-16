# `trace_init()` — Full Tracing Infrastructure

## Purpose

Completes the tracing infrastructure setup after the full memory allocator is available. Registers all built-in tracers, creates the tracefs pseudo-filesystem tree, and initializes the event tracing subsystem.

## Source File

`kernel/trace/trace.c`

## Key Operations

```c
void __init trace_init(void)
{
    trace_event_init();   // Register all TRACE_EVENT() events
    // Create global_trace with per-CPU buffers
    // Register tracers: nop, function, function_graph, wakeup, etc.
    // Set up /sys/kernel/debug/tracing/ hierarchy
}
```

## Built-in Tracers Registered

| Tracer | Description |
|--------|-------------|
| `nop` | No-op (tracing disabled) |
| `function` | Trace all function calls |
| `function_graph` | Trace function call/return with timing |
| `wakeup` | Trace maximum scheduling latency |
| `wakeup_rt` | Trace wakeup latency for RT tasks |
| `irqsoff` | Measure time with IRQs disabled |
| `preemptoff` | Measure time with preemption disabled |
| `blk` | Block I/O tracing |

## Trace Events

The `TRACE_EVENT()` macro defines structured trace points:

```c
// Example from sched/sched.h:
TRACE_EVENT(sched_switch,
    TP_PROTO(bool preempt, struct task_struct *prev, 
             struct task_struct *next),
    TP_STRUCT__entry(
        __array(  char,  prev_comm, TASK_COMM_LEN)
        __field(  pid_t, prev_pid              )
        __field(  int,   prev_prio             )
        __field(  long,  prev_state            )
        __array(  char,  next_comm, TASK_COMM_LEN)
        __field(  pid_t, next_pid              )
        __field(  int,   next_prio             )
    ),
    /* ... */
);
```

These events are accessible at `/sys/kernel/debug/tracing/events/`.

## tracefs Layout After Init

```
/sys/kernel/debug/tracing/
├── current_tracer          ← Read/write current tracer name
├── tracing_on              ← 0/1 to enable/disable
├── trace                   ← Read trace buffer
├── trace_pipe              ← Streaming trace reader
├── available_tracers       ← List of registered tracers
├── set_ftrace_filter       ← Filter functions to trace
├── set_graph_function      ← Filter for graph tracer
├── events/                 ← Trace event subsystem
│   ├── enable              ← Enable all events
│   ├── sched/
│   │   ├── sched_switch/
│   │   │   ├── enable
│   │   │   ├── format
│   │   │   └── filter
│   │   └── ...
│   └── ...
├── per_cpu/                ← Per-CPU buffers
└── options/                ← Tracer options
```

## Pre-conditions

- `early_trace_init()` complete
- `kmalloc()` available
- `vmalloc()` available

## Post-conditions

- All built-in tracers registered
- Trace event system ready
- `trace_printk()`, `trace_puts()` functional
- BPF tracing infrastructure ready

## Cross-references

- [Phase overview](../README.md)
- `early_trace_init()`: [../early_trace_init/README.md](../early_trace_init/README.md)
