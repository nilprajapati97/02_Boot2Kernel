# KMEMLEAK Memory Leak Detection Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), kernel memory leak detection

---

## 1. Concept Foundation

KMEMLEAK detects unreferenced memory allocations that leak.

Mechanism:
- track all allocations (kmalloc, vmalloc, etc.)
- scan kernel memory for references (pointers)
- report allocations with no incoming references

Use cases:
- driver development testing
- subsystem memory leak detection
- long-running kernel service validation

---

## 2. ARM64 Hardware Detail

### 2.1 Memory scanning

Scanning kernel memory for pointers is intensive.
ARM64 supports word-aligned loads for efficient scanning.

### 2.2 Pointer detection

Algorithm looks for values matching allocation addresses.
Heuristic: assumes leaked memory has no valid pointer to it.

---

## 3. Linux Kernel Implementation

### 3.1 Allocation tracking

On kmalloc/vmalloc: record address, size, caller.
Stored in rb-tree for efficient lookup.

### 3.2 Pointer scanning

kmemleak_scan() iterates:
- kernel bss, data sections
- kernel heap (kmalloc area)
- vmalloc areas
- task stacks

For each memory region: scan for pointer-sized values matching known allocations.

### 3.3 Reference tracking

Each allocation maintains reference count.
If count is 0 after scan, marked as leak.

### 3.4 Output interface

/sys/kernel/debug/kmemleak:
- read: dump leak report
- write: control (scan, clear, etc.)

---

## 4. Hardware-Software Interaction

KMEMLEAK usage:
1. enable CONFIG_DEBUG_KMEMLEAK
2. force allocation (trigger code path)
3. wait for memory to become "orphan"
4. trigger scan: echo scan > /sys/kernel/debug/kmemleak
5. read report: cat /sys/kernel/debug/kmemleak
6. analyze: what references are missing?

---

## 5. Interview Q and A

Q1: Why is KMEMLEAK useful if we have valgrind for user space?
Kernel allocations invisible to user-space tools; KMEMLEAK runs in-kernel.

Q2: What is false positive risk for KMEMLEAK?
High; pointers embedded in structs or encoded pointers may be missed.

Q3: How expensive is KMEMLEAK scanning?
Significant overhead; usually disabled in production, enabled during testing.

Q4: Can KMEMLEAK catch all leaks?
No; only detects allocations fully orphaned (no pointer path to them).

Q5: How do you reduce KMEMLEAK false positives?
Ignore certain allocation types or patterns; use kmemleak ignore command.

Q6: What is KMEMLEAK_NOT_LEAK annotation?
Explicit marker in code telling KMEMLEAK this is not actually a leak (intended reference).

---

## 6. Pitfalls and Gotchas

- Assuming KMEMLEAK detects all leaks (many encoding schemes fool it).
- Running KMEMLEAK on production (performance impact too high).
- Misinterpreting false positives as real leaks.
- Ignoring legitimate memory references that KMEMLEAK can't track.
- Not automating KMEMLEAK scanning and reporting in CI pipeline.

---

## 7. Quick Reference Table

| Command | Effect |
|---|---|
| echo scan > /sys/kernel/debug/kmemleak | start memory scan |
| cat /sys/kernel/debug/kmemleak | read leak report |
| echo clear > /sys/kernel/debug/kmemleak | clear detected leaks |

| Status | Meaning |
|---|---|
| leak | object has no reference path |
| suspected | likely leak but heuristic uncertain |
| ignored | explicitly marked to ignore |
