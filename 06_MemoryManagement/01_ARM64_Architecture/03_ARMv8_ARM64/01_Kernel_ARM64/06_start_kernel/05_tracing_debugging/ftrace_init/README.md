# `ftrace_init()` — Function Tracer Initialization

## Purpose

Processes the `__mcount_loc` section of the kernel image to build a table of all function call sites that can be dynamically traced. Patches all these sites to NOPs initially, so tracing has zero overhead when disabled.

## Source File

`kernel/trace/ftrace.c`

## The mcount Mechanism

When compiled with `-pg` (or `-mfentry` on x86), GCC inserts a call at the beginning of every function:

```asm
; Every kernel function starts with:
<some_function>:
    callq  __fentry__      ← inserted by compiler (-mfentry)
    push   %rbp
    mov    %rsp, %rbp
    ; ... actual function body ...
```

This `callq __fentry__` is the "mcount site" (5 bytes on x86-64).

## What `ftrace_init()` Does

```c
void __init ftrace_init(void)
{
    // 1. Find all mcount call sites from __mcount_loc section
    //    This section contains the address of every mcount call instruction
    
    // 2. Build the ftrace_pages hash table (addr → ftrace_record)
    ftrace_process_locs(NULL, __start_mcount_loc, __stop_mcount_loc);
    
    // 3. Patch all call sites to NOP (zero overhead when tracing off)
    ftrace_update_all_trace_funcs();  
    
    // 4. Register the function tracer with the tracer list
    register_tracer(&function_tracer);
    register_tracer(&function_graph_tracer);
}
```

## The `__mcount_loc` Section

Each object file compiled with `-mfentry` emits a pointer to each mcount call site in `__mcount_loc`:

```
__mcount_loc section (read-only, freed after init):
  [0x0000]  0xffffffff81001234  ← address of mcount call in foo()
  [0x0008]  0xffffffff81002345  ← address of mcount call in bar()
  [0x0010]  0xffffffff81003456  ← address of mcount call in baz()
  ... (100,000+ entries)
```

## Patching to NOP

After building the table, all `call __fentry__` instructions are replaced with 5 NOPs using `text_poke_bp()`:

```
Before ftrace_init():
    0xffffffff81001234: e8 xx xx xx xx   ← CALL rel32

After ftrace_init():
    0xffffffff81001234: 0f 1f 44 00 00   ← NOPL 0x0(%rax,%rax,1)
```

The kernel text is now ~10-15% faster than a debug build.

## Enabling Tracing at Runtime

```bash
# Enable function tracing for all functions:
echo function > /sys/kernel/debug/tracing/current_tracer
echo 1 > /sys/kernel/debug/tracing/tracing_on

# Trace specific function:
echo schedule > /sys/kernel/debug/tracing/set_ftrace_filter
```

When tracing is enabled, the NOP is patched back to:
```asm
callq ftrace_caller    ← routes to registered trace function
```

## Function Graph Tracer

Also registered here — traces function entry AND exit:

```
# cat /sys/kernel/debug/tracing/trace
 0)               |  schedule() {
 0)               |    __schedule() {
 0)   0.512 us    |      rq_lock();
 0)   0.234 us    |      update_rq_clock();
```

## Overhead When Disabled

- **Tracing OFF**: Single `NOPL` (1 cycle, no memory access, predicted by CPU)
- **Tracing ON**: `CALL` to ftrace_caller → lookup handler → call trace function

## Pre-conditions

- `__mcount_loc` section valid and accessible
- Kernel text section temporarily writable
- `vmalloc` available (for ftrace hash tables)

## Post-conditions

- All function call sites patched to NOP
- `ftrace_enabled = 1`
- Function and function_graph tracers registered

## Kconfig

- `CONFIG_FUNCTION_TRACER`: Enables ftrace
- `CONFIG_HAVE_FUNCTION_TRACER`: Arch support
- `CONFIG_DYNAMIC_FTRACE`: Runtime NOP→CALL patching (the efficient mode)

## Cross-references

- [Phase overview](../README.md)
- `jump_label_init()`: [../../03_parameter_parsing/jump_label_init/README.md](../../03_parameter_parsing/jump_label_init/README.md) — similar NOP-patching
- `poking_init()`: [../poking_init/README.md](../poking_init/README.md) — text patching infrastructure
