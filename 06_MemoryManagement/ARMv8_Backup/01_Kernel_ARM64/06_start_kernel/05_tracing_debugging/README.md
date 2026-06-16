# Phase 5: Tracing & Debugging Initialization

## Overview

This phase initializes the kernel's tracing, profiling, and code-patching infrastructure. These components — especially ftrace — provide visibility into kernel execution and are used by many higher-level tools (perf, BPF, SystemTap, LTTng).

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`early_trace_init()`](early_trace_init/README.md) | `kernel/trace/trace.c` | Initialize trace buffer and trace clock |
| 2 | [`ftrace_init()`](ftrace_init/README.md) | `kernel/trace/ftrace.c` | Initialize function tracer, patch mcount sites |
| 3 | [`trace_init()`](trace_init/README.md) | `kernel/trace/trace.c` | Full tracing infrastructure setup |
| 4 | [`context_tracking_init()`](context_tracking_init/README.md) | `kernel/context_tracking.c` | User/kernel context tracking |
| 5 | [`poking_init()`](poking_init/README.md) | `arch/x86/kernel/alternative.c` | Initialize text patching mutex/mm |
| 6 | `initcall_debug` check | `init/main.c` | Enable initcall tracing if requested |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled

## Memory State

- **Entry**: Full `kmalloc` available (from Phase 4)
- All `vmalloc` areas available

## What Gets Enabled Here

### ftrace (Function Tracer)

The most powerful component. `ftrace_init()` patches ~100,000+ function call sites in the kernel:

```
Before patching: Each function starts with:
    callq  0xffffffff81234567  ← calls __fentry__ or mcount
    
When tracing disabled: patched to:
    nop nop nop nop nop        ← 5-byte NOP, zero overhead

When tracing enabled: restored to:
    callq  0xffffffff81234567  ← calls ftrace hook
```

### Trace Events

`trace_init()` sets up the trace event subsystem — the infrastructure behind `trace_printk()`, BPF kprobes, and perf events.

### Context Tracking

`context_tracking_init()` enables tracking of user↔kernel transitions, used by:
- NO_HZ_FULL (CPU tick can be stopped when in user space)
- RCU extended quiescent states
- vTime (virtual time tracking for VMs)

## Key Output of Phase

- `tracefs` can be mounted at `/sys/kernel/debug/tracing/`
- `echo function > /sys/kernel/debug/tracing/current_tracer` works (after mount)
- `perf record -g` function call tracing works
- BPF kprobes can be attached

## Function Index

- [early_trace_init/](early_trace_init/README.md)
- [ftrace_init/](ftrace_init/README.md)
- [trace_init/](trace_init/README.md)
- [context_tracking_init/](context_tracking_init/README.md)
- [poking_init/](poking_init/README.md)
