# ARM64 idmap and MMU Enable Sequence Deep Dive

Category: KASLR and Kernel Boot Memory Setup  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

idmap is a temporary identity mapping used during critical transitions where virtual and physical addresses must match.

Why it exists:
- During MMU on and off transitions, execution cannot rely on final kernel virtual layout yet.
- CPU may need to execute code whose address interpretation changes mid-sequence.
- Identity map keeps instruction fetch stable across control-register transitions.

Primary use cases:
- Early boot MMU enable path
- CPU resume from low-power states
- Secondary CPU bring-up and hotplug entry paths

---

## 2. ARM64 Hardware Detail

### 2.1 Critical registers in MMU enable

Key control registers:
- TTBR0_EL1 or TTBR1_EL1 chosen for active translation regime
- TCR_EL1 for translation granule and VA size
- MAIR_EL1 for memory attributes
- SCTLR_EL1 for enabling MMU and caches

Core transition bit:
- SCTLR_EL1.M controls MMU enable state

Sequence requirements:
- Program translation configuration first.
- Ensure page-table base points to valid tables.
- Execute barrier and synchronization instructions around SCTLR writes.

### 2.2 Identity map property

Identity mapping means:
- VA equals PA for mapped transition region.
- Control flow remains valid when MMU state toggles.

Mapped region usually includes:
- transition routines
- minimal stack or data needed by those routines
- essential page tables

---

## 3. Linux Kernel Implementation

### 3.1 idmap page-table construction

Linux builds dedicated idmap structures in early boot memory code.
Concepts include:
- idmap page-table root
- minimal section coverage for transition code
- architecture helpers for creating block or page entries

### 3.2 __cpu_setup and MMU turn-on

High-level flow:
1. Configure MAIR_EL1 and TCR_EL1.
2. Point TTBR to valid early tables.
3. Write SCTLR_EL1 with M and required cache bits.
4. Execute ISB to ensure new context is active before further fetch.

ISB importance:
- Guarantees instruction stream observes updated control state.
- Prevents execution with stale decode assumptions.

### 3.3 Resume and secondary paths

Additional idmap users:
- cpu_resume style paths after suspend
- secondary CPU start paths
- hotplug onlining transitions

These paths often re-enter with constrained context and require deterministic identity-mapped code stubs.

---

## 4. Hardware-Software Interaction

MMU enable transition timeline:
1. CPU executes in temporary environment.
2. idmap tables are active and cover transition code.
3. Kernel programs translation and attributes.
4. SCTLR_EL1 enables MMU.
5. ISB serializes pipeline state.
6. Control branches into final mapped kernel virtual region.

Failure modes if idmap is wrong:
- instruction abort immediately after SCTLR write
- recursive exception loops with no valid vector mapping
- early boot hang with no console output

---

## 5. Interview Q and A

Q1: Why can we not jump directly into final kernel mapping before MMU enable?
Because final virtual addresses are not meaningful until translation is configured and activated.

Q2: Why is identity mapping preferred for transition code?
It keeps addresses stable before and after MMU state change.

Q3: What is the role of ISB after enabling MMU?
It ensures subsequent instruction fetch and execution use the new control register state.

Q4: Is idmap only a boot-time concept?
No. It is also used in resume and secondary CPU entry paths.

Q5: What register write typically flips MMU state?
SCTLR_EL1 write controlling the M bit.

Q6: What symptom indicates broken idmap coverage?
Immediate instruction abort or silent hang right after MMU enable.

---

## 6. Pitfalls and Gotchas

- Missing idmap coverage for branch target after SCTLR update.
- Wrong memory attributes for page-table memory causing inconsistent behavior.
- Forgetting synchronization barriers around control-register writes.
- Assuming primary CPU path and secondary CPU path can share identical assumptions.
- Debugging difficulty due to very early failure before full console setup.

---

## 7. Quick Reference Table

| Item | Purpose |
|---|---|
| idmap | Temporary identity-mapped execution region |
| SCTLR_EL1.M | MMU enable control bit |
| TCR_EL1 | Translation size and granule controls |
| MAIR_EL1 | Memory attribute definitions |
| ISB | Synchronize instruction stream after control-state changes |
