# Kernel Debug Build and Symbol Resolution Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), debug kernel configuration

---

## 1. Concept Foundation

Debug kernel builds include symbol information and debug flags.

Benefit:
- source-level debugging
- accurate stack traces
- precise variable inspection

Trade-off:
- larger kernel binary
- reduced performance (no optimizations)
- longer compile time

---

## 2. ARM64 Hardware Detail

### 2.1 Debug symbol format

DWARF format encodes source-to-binary mapping.
ARM64 kernels typically use DWARF-4.

### 2.2 Optimization impact

Debug builds use -O0 or -Og (minimal optimization).
More predictable behavior but slower execution.

---

## 3. Linux Kernel Implementation

### 3.1 Kernel configuration

CONFIG_DEBUG_INFO: include DWARF symbols  
CONFIG_DEBUG_KERNEL: enable kernel debug options  
CONFIG_KALLSYMS: enable kernel symbol table

### 3.2 Symbol extraction

Kernel binary contains .debug_* sections.
Tool: strip, objdump, readelf to query symbols.

### 3.3 crash dump analysis

vmcore (kernel dump) analyzed with crash tool.
Requires matching debug kernel and symbol table.

---

## 4. Hardware-Software Interaction

Debugging scenario:
1. kernel panic dumps core to disk
2. copy vmcore and matching vmlinux-dbg
3. crash vmlinux-dbg vmcore
4. inspect stack trace with source lines
5. find buggy code location

---

## 5. Interview Q and A

Q1: Why use debug kernel in production?
Usually not; use in development/testing. Production kernels optimized, not debuggable.

Q2: How do you enable DWARF in kernel?
CONFIG_DEBUG_INFO=y in kernel .config

Q3: What is kallsyms and why is it useful?
Kernel symbol table; enables oops decoder to show function names in traces.

Q4: Can you attach gdb to running kernel?
Yes, via KGDB (kernel debugger); requires serial port and careful setup.

Q5: What is the size overhead of debug symbols?
Typically 50-100% larger kernel binary; debug sections often stripped in production.

Q6: How do you correlate perf samples to source code?
Requires debug kernel and debuginfo package; perf-annotate shows source lines.

---

## 6. Pitfalls and Gotchas

- Forgetting to rebuild kernel after .config changes.
- Mismatched kernel and debug symbols (gives wrong line numbers).
- Debug kernel performance so different from production that bugs hard to reproduce.
- Not keeping debug kernel symbols backed up; easily lost.
- Assuming debug features have no overhead (they do).

---

## 7. Quick Reference Table

| Config | Effect |
|---|---|
| CONFIG_DEBUG_INFO=y | include DWARF debug symbols |
| CONFIG_KALLSYMS=y | kernel function name symbols |
| CONFIG_DEBUG_KERNEL=y | enable kernel debug options |

| Tool | Purpose |
|---|---|
| objdump | disassemble and inspect binary |
| readelf | read ELF sections |
| crash | analyze kernel dumps |
