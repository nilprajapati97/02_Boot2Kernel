# TrustZone versus ARM CCA RME Deep Dive

Category: Security Extensions  
Platform: ARM64 (AArch64), ARMv9 security evolution

---

## 1. Concept Foundation

TrustZone provides two-world isolation:
- Secure world
- Non-secure world

ARM CCA with RME extends this model toward confidential compute:
- Root world
- Secure world
- Realm world
- Non-secure world

Why this evolution exists:
- Two-world model is strong for TEE use cases but limited for cloud multi-tenant confidential workloads.
- Realm model adds a hardware-isolated execution domain not directly controlled by normal world hypervisor.

---

## 2. ARM64 Hardware Detail

### 2.1 Four security states in ARMv9 RME

Conceptual states:
- Root: highest management domain for platform and monitor components
- Secure: trusted services and legacy secure payload model
- Realm: confidential workloads with strong isolation from non-secure host
- Non-secure: host OS and conventional workloads

### 2.2 Granule Protection Checks

RME introduces granule-based protection metadata:
- Physical memory is managed in granules.
- Hardware checks ownership and access state per granule.
- Unauthorized accesses are blocked regardless of requesting software privilege level.

Key elements include control registers and tables owned by root-level firmware/monitor stack.

### 2.3 Realm management layer

Realm Management Monitor component coordinates:
- Realm creation and teardown
- Memory delegation between states
- Entry and exit protocol for realm execution
- Attestation-related state management

---

## 3. Linux, Hypervisor, and Firmware Implementation View

### 3.1 TrustZone baseline path

Legacy secure services:
- EL3 monitor and BL32 trusted OS provide secure APIs through SMC.
- Linux remains in non-secure world.

### 3.2 CCA deployment model

In CCA-enabled systems:
- Host hypervisor still manages non-secure VMs.
- Realm VMs are managed through dedicated realm interfaces.
- Host cannot directly inspect realm plaintext memory.

Software stack includes coordinated roles across:
- EL3 or root-level firmware components
- hypervisor integration
- guest support for realm execution model

### 3.3 Attestation flow relevance

Confidential workloads require remote trust evidence:
- Realm launch measurements recorded
- Attestation tokens produced via trusted monitor chain
- Remote verifier confirms platform and workload integrity before releasing secrets

---

## 4. Hardware-Software Interaction

Realm lifecycle at high level:
1. Host requests realm creation via realm management interface.
2. Memory granules are delegated from host ownership to realm ownership.
3. Realm execution starts with isolated CPU and memory context.
4. Host interacts only via controlled shared buffers and exits.
5. On teardown, granules are reclaimed through validated state transitions.

Comparison to TrustZone service call model:
- TrustZone: normal world calls secure service by SMC, secure world remains centralized.
- RME: realm guest itself is isolated execution domain with confidentiality against host.

---

## 5. Interview Q and A

Q1: Why is TrustZone not enough for cloud confidential compute?
Because secure world is typically a centralized trusted domain, while confidential cloud workloads need tenant-isolated execution not directly visible to host hypervisor.

Q2: What is the key idea of RME memory protection?
Granule ownership and hardware-enforced access checks independent of host control path.

Q3: Does RME replace TrustZone completely?
No. They can coexist. TrustZone remains useful for secure services and platform trust functions.

Q4: What is the role of attestation in CCA?
To provide cryptographic evidence of platform and realm state so remote services can decide whether to trust and provision secrets.

Q5: Can host kernel read realm memory with higher privilege?
No, not by normal means. Hardware protection and ownership checks prevent direct host access to realm-private data.

Q6: What changes for Linux operators?
New lifecycle tooling, firmware dependencies, attestation flows, and revised observability model for realm workloads.

---

## 6. Pitfalls and Gotchas

- Assuming secure world and realm world are interchangeable trust domains.
- Ignoring firmware version dependencies for CCA feature enablement.
- Underestimating operational complexity of attestation infrastructure.
- Expecting traditional host debug methods inside realm workloads.
- Mismanaging shared buffer boundaries between host and realm can create side channels.

---

## 7. Quick Reference Table

| Model | Security domains |
|---|---|
| TrustZone classic | Secure and Non-secure |
| ARM CCA with RME | Root, Secure, Realm, Non-secure |

| Feature | Benefit |
|---|---|
| Granule ownership checks | Strong memory isolation |
| Realm execution model | Confidential workload isolation from host |
| Attestation integration | Remote trust establishment |
| Coexistence with TrustZone | Backward-compatible secure services path |
