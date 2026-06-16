# Debugging and Profiling Complete Reference

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Kernel debugging is a layered discipline:
- detect correctness bugs
- trace runtime behavior
- profile performance bottlenecks
- perform postmortem analysis

Best practice is tool composition, not tool isolation.

---

## 2. ARM64 Hardware Detail

### 2.1 Relevant capabilities

- PMU for perf events
- BRK/hardware breakpoints for probing
- architecture-specific memory ordering effects for race diagnostics

### 2.2 Practical implication

ARM64 weak ordering and high core counts make race and locality issues more visible.

---

## 3. Linux Kernel Implementation

Tool stack in this category:
- KASAN/KMEMLEAK/KCSAN/UBSAN
- ftrace, kprobes, BPF, systemtap
- perf for hotspot and stall profiling
- debug symbols, crash, kdump for postmortem

---

## 4. Hardware-Software Interaction

Operational workflow:
1. detect anomaly (SLO/latency/correctness)
2. capture minimal high-value traces
3. correlate events with CPU/memory behavior
4. reproduce and verify fix
5. codify regression checks

---

## 5. Interview Q and A

Q1: First action for intermittent kernel bug?
Enable reproducible instrumentation with minimal perturbation.

Q2: Why combine sanitizers and tracing?
Sanitizers detect class of bug; tracing explains runtime path.

Q3: perf vs ftrace in one line?
perf samples hotspots; ftrace records event causality.

Q4: When is postmortem mandatory?
When panic/corruption is non-reproducible in live debugging.

Q5: How to reduce observability overhead?
Use scoped events, short windows, and trigger-based capture.

Q6: What defines a mature debugging pipeline?
Automated collection, correlation, and regression gates.

---

## 6. Pitfalls and Gotchas

- Over-instrumentation altering behavior.
- Missing symbol/debug artifact retention.
- One-off manual debugging without repeatable playbooks.
- No validation that fix eliminates root cause.

---

## 7. Quick Reference Table

| Layer | Description |
|---|---|
| Sanitizers | detect memory/race/UB defects |
| Tracing | capture event-level causality |
| Profiling | quantify hotspot/stall distribution |
| Postmortem | inspect crash state offline |
| Correlation | join signals into one causal timeline |
