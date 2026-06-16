# BPF (Extended Berkeley Packet Filter) Observability Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), BPF-based instrumentation

---

## 1. Concept Foundation

BPF is a lightweight runtime for in-kernel programs.

Uses:
- dynamic tracing (bcc tools, bpftrace)
- network filtering
- performance monitoring without code reload

Key advantage: deploy observability without kernel rebuild or downtime.

---

## 2. ARM64 Hardware Detail

### 2.1 BPF virtual machine

ARM64 kernel includes BPF JIT compiler.
BPF bytecode compiled to native ARM64 instructions for efficiency.

### 2.2 Verifier

Kernel verifier ensures BPF programs are safe (no infinite loops, no kernel memory corruption).

---

## 3. Linux Kernel Implementation

### 3.1 BPF program types

kprobe programs: run on function entry/exit  
tracepoint programs: run on kernel events  
XDP programs: network packet processing

### 3.2 eBPF (extended BPF)

Modern version with more instructions and better performance.
Maps for passing data to user space.

### 3.3 BPF maps

Kernel-user communication: BPF programs write to map, user space reads.
Supports arrays, hashmaps, ring buffers.

---

## 4. Hardware-Software Interaction

BPF tracing example:
1. write BPF program that samples memory allocation
2. load BPF program via bpf syscall
3. attach to kprobe or tracepoint
4. program runs in kernel on event
5. data written to map
6. user space polls map and processes results

---

## 5. Interview Q and A

Q1: Why use BPF instead of kprobes?
BPF is more flexible (can write custom logic) and efficient (in-kernel filtering).

Q2: How is BPF safer than loadable kernel modules?
BPF verifier ensures program doesn't crash kernel or access invalid memory.

Q3: What is bpftrace and how does it use BPF?
bpftrace is high-level syntax for writing BPF programs; simplifies dynamic tracing.

Q4: Can BPF programs cause performance issues?
Yes, if run frequently or do expensive operations; careful program design essential.

Q5: What is BPF ring buffer and when is it used?
Ring buffer for low-overhead event streaming; alternative to maps for high-frequency data.

Q6: How do you debug BPF programs?
Limited debugging; rely on prints via bpf_printk() and verifier feedback.

---

## 6. Pitfalls and Gotchas

- Writing BPF programs that fail verifier (learning curve).
- Running expensive BPF on high-frequency events (causes slowdown).
- Forgetting to pin BPF programs (data lost after unload).
- Mixing BPF program types incorrectly.
- Not testing BPF programs on representative hardware.

---

## 7. Quick Reference Table

| Program type | Trigger |
|---|---|
| kprobe | function entry/exit |
| tracepoint | kernel event |
| XDP | network packet arrival |

| Tool | Use case |
|---|---|
| bcc | Python-based BPF programs |
| bpftrace | one-liner dynamic tracing |
| perf-trace | kernel event tracing with BPF |
