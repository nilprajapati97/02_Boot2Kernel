# kprobes and Kernel Instrumentation Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), dynamic kernel instrumentation

---

## 1. Concept Foundation

kprobes enable inserting instrumentation code at any kernel function without modifying source.

Use cases:
- tracing function arguments and returns
- conditional sampling
- dynamically patching behavior

---

## 2. ARM64 Hardware Detail

### 2.1 Instruction replacement

kprobes replaces first instruction of target function with breakpoint (BRK instruction on ARM64).
On breakpoint, handler runs user code.

### 2.2 ARM64 breakpoint support

ARM64 supports both software breakpoints and hardware breakpoints.
Software (BRK) simpler; hardware more efficient but limited count.

---

## 3. Linux Kernel Implementation

### 3.1 kprobes registration

Users register kprobe structure with target address.
Kernel patches function entry with breakpoint.

### 3.2 Probe handler

On breakpoint hit:
1. CPU exception (SIGTRAP equivalent)
2. exception handler redirects to kprobe handler
3. user code runs in atomic context
4. original instruction executed (single-stepped)
5. execution continues

### 3.3 Return probes (kretprobes)

Wrapper around kprobes to capture return values.
Registers probe on function entry and exit.

---

## 4. Hardware-Software Interaction

kprobe example:
1. register kprobe on vmalloc_node()
2. set handler to log allocation size
3. trigger allocation
4. breakpoint hit, handler runs
5. logs size, allows execution to continue

---

## 5. Interview Q and A

Q1: How does kprobe differ from replacing code with printk?
kprobes are dynamic (no rebuild needed) and efficient (conditional sampling).

Q2: Can you kprobe arbitrary functions?
Mostly yes, except some critical paths that can't handle exceptions safely.

Q3: What is the performance cost of kprobes?
Minimal when not triggered; when triggered, some overhead from breakpoint handling.

Q4: Can kprobes access function arguments?
Yes; register layout known, arguments extracted from registers/stack.

Q5: What is kretprobe and how does it work?
kretprobe captures function return values; uses wrapper to intercept both entry and exit.

Q6: Can you use kprobes in production?
Yes, but carefully; avoid high-frequency functions to minimize overhead.

---

## 6. Pitfalls and Gotchas

- Probing functions with too high frequency (causes system slowdown).
- Handler code that is not re-entrant (causes deadlock).
- Accessing invalid memory in probe handler (kernel crash).
- Forgetting to unregister probes (resource leak).
- Not testing probes thoroughly (break kernel in production).

---

## 7. Quick Reference Table

| Tool | Use case |
|---|---|
| kprobe | instrument function entry |
| kretprobe | capture return value |
| jprobe | call handler with function args (deprecated) |

| Interface | Purpose |
|---|---|
| /sys/kernel/debug/kprobes | list active probes |
| trace_kprobe | kprobe integration with ftrace |
