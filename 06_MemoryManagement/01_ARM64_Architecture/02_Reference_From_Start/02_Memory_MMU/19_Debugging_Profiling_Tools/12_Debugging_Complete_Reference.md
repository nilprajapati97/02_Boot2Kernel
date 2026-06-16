# Debugging Complete Reference and Workflow Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), comprehensive debugging toolkit

---

## 1. Concept Foundation

Effective debugging requires combining multiple tools.

Layers:
- compile-time checkers (UBSAN, KCSAN)
- runtime detectors (KMEMLEAK, KASAN)
- event tracing (ftrace, BPF)
- profiling (perf)
- dynamic instrumentation (kprobes, systemtap)
- offline analysis (crash, gdb)

---

## 2. ARM64 Hardware Detail Summary

ARM64 provides PMU, breakpoint units, and efficient tracing infrastructure.
Weak memory ordering makes some bug classes (races) easier to observe.

---

## 3. Linux Kernel Implementation Summary

Debugging infrastructure is comprehensive:
- in-kernel sanitizers (KASAN, UBSAN, KCSAN)
- event tracing (ftrace, trace-cmd)
- dynamic instrumentation (kprobes, BPF)
- performance analysis (perf)
- offline debugging (gdb, crash)

---

## 4. Hardware-Software Interaction: Complete Picture

Debugging workflow:
1. reproduce issue on debug kernel
2. enable relevant sanitizers or tracers
3. collect data (ftrace, perf, BPF)
4. analyze results to pinpoint bug
5. correlate with source code (symbols)
6. instrument to refine understanding
7. fix and verify

---

## 5. Interview Q and A

Q1: How do you choose among ftrace, perf, and BPF?
ftrace: simplest for event tracing. perf: best for profiling. BPF: flexible for custom logic.

Q2: What is the first step in debugging kernel issue?
Reproduce consistently; enable CONFIG_DEBUG_KERNEL and relevant sanitizers.

Q3: Why run sanitizers early in development?
KASAN/UBSAN/KCSAN catch bugs immediately; much easier than post-mortem analysis.

Q4: How do you debug without source code?
Still possible: objdump for disassembly, perf for sampling, but much harder.

Q5: What is the typical bottleneck in kernel debugging?
Reproducing issue consistently; once reproducible, tracing usually identifies cause.

Q6: How do you make debugging faster?
Automate collection and analysis; use scripting (bpftrace) to narrow issue scope.

---

## 6. Pitfalls and Gotchas

- Enabling too many debug options and impacting performance.
- Collecting too much data (huge traces hard to analyze).
- Not backing up debug symbols and losing them.
- Forgetting that debug kernel behavior differs from production.
- Assuming single tool will find all issues (usually need combination).

---

## 7. Complete Debugging Stack

| Phase | Tools |
|---|---|
| Compile-time | UBSAN, compiler warnings, static analysis |
| Runtime sanitizers | KASAN, KMEMLEAK, KCSAN |
| Event collection | ftrace, BPF, kprobes |
| Profiling | perf, systemtap |
| Offline analysis | crash, gdb, objdump |

| Workflow | Typical steps |
|---|---|
| memory issue | enable KASAN → collect trace → analyze pattern → fix |
| performance | perf record → perf report → identify hotspot → optimize |
| race condition | enable KCSAN → reproduce race → inspect code → add synchronization |
