# `early_trace_init()` — Early Trace Buffer Setup

## Purpose

Initializes the trace ring buffer and trace clock before the full tracing infrastructure is ready. This allows kernel messages during early boot to be captured for later analysis.

## Source File

`kernel/trace/trace.c`

```c
void __init early_trace_init(void)
{
    if (tracepoint_printk) {
        tracepoint_print_iter =
            kzalloc(sizeof(*tracepoint_print_iter), GFP_KERNEL);
        /* ... */
    }
    tracer_alloc_buffers();
    trace_event_init();
}
```

## Components Initialized

### Trace Ring Buffer

Allocates the per-CPU ring buffers used to store trace records:

```c
struct trace_array {
    struct ring_buffer *array_buffer.buffer;  // Per-CPU ring buffers
    /* ... */
};
```

Default buffer size: 4MB per CPU (configurable with `trace_buf_size=` cmdline).

### Trace Clock

Selects the timestamp source for trace records:

| Clock | Accuracy | Description |
|-------|----------|-------------|
| `local` | ~100ns | `sched_clock()` — per-CPU, fast |
| `global` | ~100ns | Globally consistent |
| `counter` | N/A | Just a counter (for ordering) |
| `uptime` | ms | Seconds since boot |
| `perf` | ns | perf clock (most accurate) |

### Trace Event Subsystem Init

Registers the built-in trace event handlers (for `TRACE_EVENT()` macros used throughout the kernel).

## Pre-conditions

- `kmalloc()` available
- Per-CPU areas set up

## Post-conditions

- Trace buffer allocated
- `trace_printk()` functional
- Early boot messages can be captured

## Cross-references

- [Phase overview](../README.md)
- `trace_init()`: [../trace_init/README.md](../trace_init/README.md) — full init
- `ftrace_init()`: [../ftrace_init/README.md](../ftrace_init/README.md) — function tracing
