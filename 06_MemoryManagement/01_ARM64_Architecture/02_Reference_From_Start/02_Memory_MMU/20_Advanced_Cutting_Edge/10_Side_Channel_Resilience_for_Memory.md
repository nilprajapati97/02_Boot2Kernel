# Side-Channel Resilience for Memory Subsystems Deep Dive

Category: Advanced and Cutting-Edge Topics  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Confidential memory protects plaintext access but not all side channels.

Major classes:
- cache timing leakage
- branch predictor effects
- page-fault/access-pattern leakage

---

## 2. ARM64 Hardware Detail

### 2.1 Microarchitectural channels

Shared last-level caches and predictor structures can leak cross-context patterns.

### 2.2 Mitigation primitives

Barrier instructions, predictor controls, and partitioning policies reduce leakage surface.

---

## 3. Linux Kernel Implementation

### 3.1 Isolation techniques

- CPU/core pinning
- cache/QoS partitioning where available
- restricted shared-memory interfaces

### 3.2 Event hardening

Kernel paths reduce data-dependent timing in sensitive code and limit fine-grained observability exposure.

---

## 4. Hardware-Software Interaction

System hardening requires matching policy to workload threat model; aggressive mitigation can reduce throughput, so practical deployment uses tiered controls.

---

## 5. Interview Q and A

Q1: Does memory encryption stop timing attacks?
No, timing/access-pattern channels can still leak information.

Q2: Best first mitigation?
Strong isolation and minimized sharing boundaries.

Q3: Why is page-fault leakage relevant?
Fault patterns can reveal secret-dependent access behavior.

Q4: How to balance perf and security?
Apply strongest controls to high-sensitivity workloads only.

Q5: Can software fully solve side channels?
Not fully; needs hardware and firmware cooperation.

Q6: Validation method?
Adversarial benchmarking plus telemetry review.

---

## 6. Pitfalls and Gotchas

- Treating confidentiality features as complete side-channel defense.
- Overlooking co-tenancy effects.
- Using one mitigation globally without threat-tiering.
- No red-team validation.

---

## 7. Quick Reference Table

| Risk | Mitigation |
|---|---|
| Cache timing | partitioning, affinity, reduced sharing |
| Predictor leakage | predictor controls and barriers |
| Fault-pattern leakage | access-pattern hardening |
| Telemetry leakage | coarse-grained observability exposure |
