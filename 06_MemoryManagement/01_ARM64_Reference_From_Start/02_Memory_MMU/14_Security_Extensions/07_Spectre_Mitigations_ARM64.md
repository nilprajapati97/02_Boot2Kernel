# Spectre Mitigations on ARM64 Deep Dive

Category: Security Extensions  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Spectre attacks abuse branch prediction and speculative execution to leak data through side channels.

Main classes relevant to ARM64:
- Spectre v1 (bounds check bypass)
- Spectre v2 (branch target injection)
- Spectre BHB/CSV2 family (history buffer influence variants)

Core idea:
- Architectural permission checks still work.
- Speculation executes transiently and leaves cache state artifacts.
- Attack reads those artifacts to infer secret data.

Mitigation strategy:
- Constrain speculation around sensitive code.
- Harden indirect branch behavior.
- Use firmware and CPU feature controls where available.

---

## 2. ARM64 Hardware Detail

### 2.1 Relevant controls and instructions

Common primitives:
- CSDB: consume speculation barrier pattern used with masking
- SB: stronger speculation barrier on supporting CPUs
- ISB/DSB: ordering barriers, not full substitutes for all Spectre cases
- CLRBHB: clear branch history buffer on supporting cores

Feature discovery registers include fields in:
- ID_AA64PFR0_EL1
- ID_AA64ISAR1_EL1
- ID_AA64ISAR2_EL1

### 2.2 Spectre v1 mechanics

Pattern:
1. Branch predicts bounds check as true.
2. Out-of-bounds load executes transiently.
3. Secret-derived index touches cache line.
4. Attacker times cache to infer secret.

ARM64 software hardening:
- index masking and array_index_nospec style patterns
- CSDB sequences to prevent value mis-speculation from propagating

### 2.3 Spectre v2 mechanics

Pattern:
- Attacker poisons indirect branch predictor.
- Victim executes indirect branch and transiently jumps to gadget.

Mitigations:
- Branch predictor hardening through firmware hooks
- Context synchronization and prediction invalidation sequences
- BTI and PAC can reduce useful gadget quality but are not full v2 replacement

---

## 3. Linux Kernel Implementation

### 3.1 Core files and reporting

Key source path:
- arch/arm64/kernel/spectre.c

User-visible status:
- /sys/devices/system/cpu/vulnerabilities/spectre_v1
- /sys/devices/system/cpu/vulnerabilities/spectre_v2
- Related entries for BHB-style variants depending on kernel version

Boot logs summarize:
- detected vulnerability class
- selected mitigation path
- firmware dependency state

### 3.2 Spectre v1 software hardening

Typical kernel patterns:
- bounds checks with masking
- nospec helpers around attacker-controlled indices
- careful handling in copy_to_user or array-indexed tables

Conceptual sequence:
1. Validate index.
2. Derive masked safe index.
3. Insert CSDB-safe sequence where needed.
4. Access array using safe index.

### 3.3 Spectre v2/BHB mitigation paths

Mitigation sources:
- Firmware-mediated branch predictor hardening calls
- CPU-specific alternatives patched at boot
- Optional use of CLRBHB on supported microarchitectures
- Scheduler and exception-entry hooks to reduce predictor cross-domain influence

Kernel alternatives framework picks best sequence per CPU capability at runtime.

---

## 4. Hardware-Software Interaction

Example kernel entry hardening path:
1. Exception entry from EL0.
2. Entry assembly executes predictor hardening sequence (CPU dependent).
3. Kernel C handlers run with reduced predictor poisoning risk.
4. Exit paths may include symmetric cleanup depending on configuration.

Example v1-hardened data access:
1. Untrusted index arrives from syscall argument.
2. Kernel masks index to legal range.
3. Barrier sequence blocks unsafe transient dependency propagation.
4. Data fetch occurs on sanitized index only.

Residual risk model:
- Some mitigations are best-effort and microarchitecture-specific.
- Firmware quality and revision level materially affect real protection.

---

## 5. Interview Q and A

Q1: Why is Spectre harder than Meltdown to fully fix?
Because Spectre exploits generic prediction behavior rather than a single permission-check timing bug. Mitigations are distributed across compiler, kernel, firmware, and CPU features.

Q2: What does array_index_nospec style logic do?
It ensures attacker-controlled indices are masked so speculative paths cannot index outside intended bounds.

Q3: Is BTI enough for Spectre v2?
No. BTI helps constrain valid branch targets, but v2 mitigation also needs predictor hardening and CPU-specific controls.

Q4: Where do you check runtime status on Linux?
In files under /sys/devices/system/cpu/vulnerabilities and dmesg boot output.

Q5: What is CLRBHB used for?
It clears branch history state to reduce BHB-style cross-context predictor influence on supported cores.

Q6: Are barriers like DSB and ISB universal Spectre fixes?
No. They are ordering tools. Spectre mitigation requires targeted speculation controls, not only memory-order barriers.

---

## 6. Pitfalls and Gotchas

- Assuming one mitigation covers all Spectre variants is incorrect.
- Ignoring firmware dependency can leave systems partially mitigated.
- Excessive barrier insertion can heavily degrade performance.
- Variant naming differs across kernel versions and vendor advisories.
- Benchmarking without workload classification hides real mitigation costs.

---

## 7. Quick Reference Table

| Variant | Core issue | Typical mitigation |
|---|---|---|
| Spectre v1 | Bounds-check bypass | Masking plus CSDB style hardening |
| Spectre v2 | Indirect branch poisoning | Predictor hardening, firmware hooks, BTI assist |
| BHB class | Branch history influence | Entry hardening and CLRBHB where available |

| Where to inspect | Purpose |
|---|---|
| arch/arm64/kernel/spectre.c | Kernel mitigation selection logic |
| /sys/devices/system/cpu/vulnerabilities | Runtime mitigation status |
| dmesg | Boot-time mitigation decisions |
