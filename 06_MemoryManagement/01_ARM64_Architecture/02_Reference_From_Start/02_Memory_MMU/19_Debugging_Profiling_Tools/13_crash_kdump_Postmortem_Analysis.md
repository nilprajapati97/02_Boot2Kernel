# crash and kdump Postmortem Analysis Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Postmortem debugging analyzes kernel state after panic using memory dumps.

Components:
- kdump captures vmcore from crashed kernel
- crash tool inspects vmcore with matching vmlinux symbols

---

## 2. ARM64 Hardware Detail

### 2.1 Crash kernel reservation

A reserved memory region (crashkernel=) is kept for second kernel boot.

### 2.2 Exception context

ARM64 stores register state at panic; crash analysis reconstructs task/CPU state.

---

## 3. Linux Kernel Implementation

### 3.1 Capture flow

1. primary kernel panics
2. kexec boots crash kernel
3. crash kernel writes vmcore to disk/network
4. reboot/analysis phase begins

### 3.2 Analysis flow

Using crash:
- open vmcore + vmlinux
- inspect backtraces, tasks, slab, page tables
- identify faulting path and memory corruption signs

---

## 4. Hardware-Software Interaction

Practical sequence:
1. enable kdump in production-safe profile
2. panic occurs under real workload
3. vmcore preserved
4. offline analysis avoids non-deterministic repro loops

---

## 5. Interview Q and A

Q1: Why use kdump if repro exists?
It captures exact failing state, including transient corruption.

Q2: Biggest requirement for usable vmcore?
Matching unstripped vmlinux with correct symbols.

Q3: What does crash do that gdb alone often cannot?
Kernel-aware commands for tasks, slabs, page tables, and locks.

Q4: Can vmcore be huge?
Yes; filtering/compression strategy is important operationally.

Q5: Why reserve crashkernel memory?
To guarantee memory for dump collection after panic.

Q6: Common first command in crash?
bt, ps, and kmem summaries for quick triage.

---

## 6. Pitfalls and Gotchas

- Symbol mismatch between vmcore and vmlinux.
- No crashkernel reservation configured.
- Dump path unavailable at panic time.
- Overwriting vmcore during automated reboot loops.

---

## 7. Quick Reference Table

| Component | Description |
|---|---|
| kdump | crash capture mechanism via kexec |
| vmcore | captured kernel memory image |
| crash | postmortem analysis tool |
| crashkernel= | boot param reserving dump kernel memory |
