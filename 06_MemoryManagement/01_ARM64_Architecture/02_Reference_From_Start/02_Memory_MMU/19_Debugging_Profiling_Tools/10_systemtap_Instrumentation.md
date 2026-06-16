# systemtap Dynamic Instrumentation Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), systemtap tracing framework

---

## 1. Concept Foundation

systemtap is high-level scripting language for dynamic kernel tracing.

Approach:
- write script describing what to trace
- systemtap compiles to kernel module
- load dynamically (no reboot)
- collect data with minimal overhead

---

## 2. ARM64 Hardware Detail

### 2.1 Kernel module compilation

systemtap generates and compiles kernel module for ARM64.
Requires kernel build environment and matching kernel headers.

### 2.2 Architecture-specific probes

ARM64 specific: UNPREDICTABLE instruction semantics, register conventions.
systemtap aware of ARM64 ABI for argument extraction.

---

## 3. Linux Kernel Implementation

### 3.1 Probe types

statement probes: arbitrary source code location  
function probes: function entry/return  
kernel events: traced syscalls, page faults, etc.

### 3.2 systemtap to BPF compilation

Modern systemtap can compile to BPF (avoiding kernel module build).
More portable and safer.

### 3.3 Output handling

Probes collect data and output via print statements.
Aggregations possible for summary statistics.

---

## 4. Hardware-Software Interaction

systemtap script example:
```
probe syscall.mmap {
  printf("mmap from %s (pid=%d)\n", execname(), pid())
}
```
1. compile to module
2. load into kernel
3. on each mmap syscall, printf runs
4. output to /var/log/messages or console

---

## 5. Interview Q and A

Q1: When should you use systemtap vs kprobes?
systemtap: higher-level, easier for complex tracing. kprobes: lower-level, more direct.

Q2: Why use systemtap vs bpftrace?
systemtap: more powerful scripting language. bpftrace: simpler, faster iteration.

Q3: What is the performance impact of systemtap?
Moderate to high depending on probe frequency and script complexity.

Q4: Can you modify kernel behavior with systemtap?
Yes, probes can modify variables or invoke functions (statementtape mode).

Q5: How do you distribute systemtap traces across team?
Compile to BPF for portability; easier than distributing kernel modules.

Q6: What is TLS (Thread Local Storage) in systemtap context?
Variables in probes can be TLS to avoid conflicts across concurrent executions.

---

## 6. Pitfalls and Gotchas

- Complex scripts causing probe handler to be slow.
- Forgetting to unload systemtap modules after use.
- Assuming systemtap works across different kernel versions (often needs adjustment).
- Writing unsafe probes that access invalid memory.
- Over-aggressive output (printk spam causing kernel slowdown).

---

## 7. Quick Reference Table

| Command | Purpose |
|---|---|
| stap script.stp | compile and load systemtap script |
| stap -L | list available probes |
| stap -e 'probe sys_open...' | one-liner probe |

| Probe type | Syntax |
|---|---|
| function entry | probe kernel.function("function_name") |
| function return | probe kernel.function("function_name").return |
| syscall | probe syscall.syscall_name |
