# KPTI on ARM64 — Meltdown-Class Mitigation Deep Dive

Category: Security Extensions  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

KPTI (Kernel Page Table Isolation) separates user and kernel virtual mappings so EL0 execution cannot speculatively observe privileged kernel mappings.

Why it exists:
- Meltdown-style attacks exploit speculative execution to read privileged mappings that should fault architecturally.
- If kernel text/data are mapped in the same TTBR1 context while user code runs, microarchitectural leakage may reveal kernel data.
- KPTI reduces attack surface by unmapping most kernel regions while running in user mode.

ARM64 context:
- Not all ARM64 cores are affected by Meltdown variants.
- Linux still supports KPTI policy and related isolation paths for vulnerable cores and hardening scenarios.

Security idea:
- User mode runs with minimal kernel mapping (trampoline, vectors, entry code only).
- On exception entry, kernel switches to full kernel page tables.
- On return to EL0, kernel switches back to user view.

---

## 2. ARM64 Hardware Detail

### 2.1 TTBR split and ASID model

ARM64 stage-1 translation uses:
- TTBR0_EL1: usually user mappings
- TTBR1_EL1: usually kernel mappings

With KPTI, Linux maintains split views:
- User view: minimal privileged mappings visible during EL0 execution
- Kernel view: full privileged mappings during EL1 execution

Relevant controls:
- TCR_EL1.A1 controls ASID source (TTBR0 or TTBR1)
- TCR_EL1.T0SZ/T1SZ define VA region size for user and kernel halves
- PAN and UAO complement KPTI but do not replace it

### 2.2 Exception entry/return interaction

On EL0 to EL1 transition:
- Hardware saves return PC in ELR_EL1 and state in SPSR_EL1
- Vector entry executes in a controlled mapping region
- Kernel quickly installs full EL1 page-table context

On EL1 to EL0 transition:
- Kernel restores user-visible translation context
- ERET returns to user with isolated map active

### 2.3 TLB implications

Context switching page-table roots and ASIDs requires careful TLB discipline:
- Avoid stale privileged translations visible to user context
- Use targeted TLBI sequences where needed
- Keep hot entry trampoline mapped globally for fast entry

---

## 3. Linux Kernel Implementation

### 3.1 Key structures and directories

Key paths:
- arch/arm64/mm/
- arch/arm64/kernel/entry.S
- arch/arm64/kernel/cpufeature.c
- arch/arm64/include/asm/pgtable.h

Main concepts:
- swapper_pg_dir: full kernel mapping
- tramp_pg_dir: minimal trampoline/user-safe mapping
- Entry trampoline code: tiny trusted path for EL0 to EL1 transition

### 3.2 Boot-time enablement

KPTI policy is selected based on:
- CPU vulnerability capability bits
- Kernel config options, including unmap-at-EL0 style options
- Boot parameters that can force or disable mitigation

Typical flow:
1. Detect CPU vulnerability class.
2. If required, allocate/build trampoline page tables.
3. Install vectors and trampoline aliases.
4. Use entry/exit stubs to switch translation context.

### 3.3 Entry code behavior

High-level entry path:
1. EL0 exception enters vector page mapped in both views.
2. Early assembly switches from user-safe map to full kernel map.
3. Full kernel handler runs syscall/interrupt/fault logic.
4. Exit path switches back before ERET.

The trampoline must avoid touching unmapped kernel data before the switch completes.

### 3.4 ASID strategy and reduced flushes

Linux uses ASID-aware context management to avoid global flushes:
- Different ASID generations reduce collision risk.
- Kernel and user contexts can use paired handling so transitions are cheap.
- Some implementations use ASID bit tricks to distinguish KPTI views.

---

## 4. Hardware-Software Interaction

End-to-end syscall with KPTI:
1. EL0 executes SVC.
2. CPU vectors to EL1 entry mapping (present in trampoline set).
3. Entry code switches to full kernel translation base.
4. Kernel services syscall with full mappings.
5. Exit code sanitizes state and switches back to user-safe map.
6. ERET to EL0.

Performance tradeoff:
- Extra instructions and occasional TLB effects on every user-kernel transition.
- Most visible in syscall-heavy and interrupt-heavy workloads.
- Less visible in compute-bound user workloads.

Security gain:
- EL0 speculative window no longer sees broad kernel mappings.
- Reduces exploitability of transient execution bugs that rely on mapped kernel pages.

---

## 5. Interview Q and A

Q1: Is KPTI always required on ARM64?
No. It is mainly needed for CPUs affected by Meltdown-class leakage. Linux enables it based on vulnerability detection and policy.

Q2: Why is trampoline code needed?
Because full kernel text is not mapped in user view. Entry code must start in a small mapping that exists in both contexts, then switch safely.

Q3: What is the difference between swapper_pg_dir and tramp_pg_dir?
swapper_pg_dir contains full kernel mappings. tramp_pg_dir contains minimal mappings required for exception vectors and transition stubs.

Q4: Does PAN make KPTI unnecessary?
No. PAN prevents direct privileged access to user mappings. KPTI removes kernel mappings from user execution context. They defend different surfaces and are complementary.

Q5: What is the main cost of KPTI?
Higher cost per EL0 to EL1 transition due to mapping/context switch overhead and additional TLB pressure.

Q6: How does Linux expose status?
Through vulnerability reporting files under /sys/devices/system/cpu/vulnerabilities and kernel logs at boot.

---

## 6. Pitfalls and Gotchas

- Accessing kernel symbols before trampoline switch in entry code causes faults.
- Overusing global TLB invalidation can regress performance sharply.
- Debugging early entry code is difficult because mapping context is intentionally minimal.
- Misconfigured vector aliases can break exception handling under load.
- Benchmarks must separate syscall-heavy versus CPU-bound workloads when evaluating KPTI overhead.

---

## 7. Quick Reference Table

| Item | Meaning |
|---|---|
| KPTI | Kernel mappings isolated from user execution context |
| swapper_pg_dir | Full kernel page table root |
| tramp_pg_dir | Minimal entry/trampoline mapping |
| Entry trampoline | Shared mapping path to switch to full kernel map |
| Main benefit | Limits speculative leakage of kernel mappings |
| Main cost | Extra work per user-kernel transition |
