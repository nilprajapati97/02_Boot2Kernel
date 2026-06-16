# Security Extensions on ARM64 Complete Reference

Category: Security Extensions  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

ARM64 platform security is layered. No single feature is enough.

Layered model:
1. Control-flow integrity: PAC and BTI
2. Memory safety hardening: MTE
3. Privileged mapping isolation: KPTI plus PAN
4. Speculation defenses: Spectre-class mitigations
5. Boot trust and world isolation: TF-A, secure boot, OP-TEE, TZASC
6. Future confidential compute: CCA and RME

Interview framing:
- Explain each feature by threat model, cost, and where it sits in stack.
- Show how features compose, not compete.

---

## 2. ARM64 Hardware Detail Snapshot

Feature discovery examples:
- ID_AA64PFR1_EL1.BT for BTI capability
- ID_AA64PFR1_EL1.MTE for MTE capability
- Pointer authentication support fields in architecture feature registers

Critical control registers used in this category:
- SCTLR_EL1: BT0 or BT1, MTE control fields on supporting cores
- TCR_EL1 and MAIR_EL1: memory attribute and translation controls supporting tagged memory policy
- SCR_EL3, SPSR_EL3, ELR_EL3: secure handoff controls

Security-state domain controls are reinforced by:
- firmware world-switch logic
- interconnect memory firewalls
- policy-driven secure boot key hierarchy

---

## 3. Linux Implementation Map

Kernel and firmware areas to know:
- arch/arm64/kernel/entry.S
- arch/arm64/kernel/spectre.c
- arch/arm64/mm/
- arch/arm64/include/asm/
- TF-A boot stages and platform port code
- drivers/tee/optee/

User-visible runtime inspection:
- /sys/devices/system/cpu/vulnerabilities
- dmesg for mitigation selection
- process and mmap attributes for BTI or MTE-enabled userspace

Core config themes:
- pointer authentication and BTI kernel hardening options
- KPTI policy and vulnerability-driven enablement
- MTE and tagged address support paths
- OP-TEE and secure monitor interface support

---

## 4. Hardware-Software Interaction Summary

Unified flow from reset to runtime:
1. ROM and early firmware establish trust anchor.
2. BL2 authenticates later stages and loads BL31 and optional BL32.
3. BL31 initializes EL3 monitor, security state routing, and handoff policy.
4. BL33 boots Linux in non-secure context.
5. Linux enables mitigation set based on CPU capabilities and config.
6. Runtime security operations use SMC transitions and strict memory-domain controls.

Cross-feature relationships:
- PAC protects returns, BTI protects indirect branch targets.
- MTE detects temporal or spatial memory misuse at granule level.
- KPTI limits mapped kernel exposure to user speculation windows.
- Spectre mitigations target predictor abuse not solved by paging permissions.

---

## 5. Top Interview Q and A

Q1: PAC versus BTI in one sentence?
PAC authenticates pointers, mainly return addresses and signed control pointers; BTI validates indirect branch landing pads.

Q2: Why do we still need KPTI if PAN exists?
PAN blocks privileged access to user mappings, while KPTI removes most kernel mappings from user execution context. They are complementary.

Q3: What does MTE add compared to classic sanitizers?
Hardware tag checks on memory accesses with low overhead and production-friendly deployment paths.

Q4: Where is TrustZone enforced beyond CPU privilege checks?
At world-state controls and system interconnect or memory firewalls such as TZASC class controllers.

Q5: How do you verify mitigation status on Linux?
Inspect vulnerability files in /sys/devices/system/cpu/vulnerabilities and boot logs.

Q6: Why is secure boot chain design operationally hard?
Because key lifecycle, revocation, rollback control, and update reliability must remain consistent across manufacturing and field updates.

Q7: What role does OP-TEE play in this stack?
Provides secure-world service execution and key handling isolated from normal-world Linux.

Q8: Is CCA replacing TrustZone immediately?
No. CCA extends isolation model for confidential workloads; TrustZone remains widely used for secure services and platform roots.

Q9: Best short explanation of Spectre mitigation complexity?
It requires coordinated compiler, kernel, firmware, and CPU support because predictor behavior is shared microarchitecture state.

Q10: What is the practical performance budget concern in this category?
Transition-heavy paths such as syscalls, exceptions, and SMC calls are most sensitive to mitigation overhead.

---

## 6. Pitfalls and Gotchas

- Treating one mechanism as universal security coverage.
- Ignoring firmware dependency when evaluating kernel mitigation claims.
- Confusing secure state isolation with standard EL privilege checks.
- Skipping rollback prevention in secure boot planning.
- Enabling hardening features without workload-aware performance testing.

---

## 7. Quick Reference Table

| Feature | Primary threat addressed | Typical cost |
|---|---|---|
| PAC | Return and pointer forgery | Low to moderate codegen overhead |
| BTI | JOP or COP gadget targeting | Very low runtime overhead |
| MTE | Memory safety violations | Low hardware-assisted cost |
| KPTI | Meltdown-class mapped-kernel leakage | Transition-path overhead |
| Spectre mitigations | Predictor-based transient leaks | Workload and CPU dependent |
| Secure boot chain | Unauthorized boot-stage code | Boot complexity and key ops |
| OP-TEE | Sensitive service isolation | SMC and marshaling overhead |
| CCA and RME | Confidential workload isolation | Platform and ecosystem complexity |

| Decision hint | Preferred feature focus |
|---|---|
| Control-flow attacks | PAC plus BTI |
| Memory corruption detection | MTE and sanitizer support |
| Boot integrity | Chain-of-trust and rollback controls |
| Trusted service isolation | OP-TEE and secure carveouts |
| Confidential multi-tenant workloads | CCA and realm model |
