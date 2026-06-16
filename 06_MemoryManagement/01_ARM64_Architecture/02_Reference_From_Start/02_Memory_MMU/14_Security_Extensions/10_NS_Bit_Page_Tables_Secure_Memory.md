# NS Bit in ARM64 Page Tables and Secure Memory Model

Category: Security Extensions  
Platform: ARM64 (AArch64), TrustZone systems

---

## 1. Concept Foundation

NS (Non-secure) controls determine whether an access belongs to secure or non-secure world.

Key point:
- Exception levels define privilege.
- Security state defines world partition.
- You can be privileged and still non-secure.

Why this matters:
- Interview confusion often mixes EL and security state.
- Secure memory isolation depends on both world state and memory controller policy.

---

## 2. ARM64 Hardware Detail

### 2.1 Security state origin

Security state is controlled from EL3 context management and SCR_EL3 policy.
- SCR_EL3.NS selects non-secure or secure target when handing off from EL3.
- Secure monitor saves/restores separate secure and non-secure contexts.

### 2.2 NS semantics in translation and attributes

Across ARM systems, NS-related semantics appear in translation descriptors and table-walk context.
Important concepts:
- Stage-1 translation context belongs to current world state.
- Stage-2 and virtualization controls can carry security-related extension bits on supporting implementations.
- Actual availability and exact bit placement may vary with architecture revision and feature set.

Practical model for interviews:
1. World state chosen by EL3 monitor.
2. Translation regime runs in that state.
3. Downstream interconnect/memory controllers enforce secure/non-secure policy for physical transactions.

### 2.3 TTBR usage in secure world

Secure EL1 software such as OP-TEE typically uses secure translation roots:
- Secure TTBR0_EL1 and TTBR1_EL1 context for secure VA space
- Separate page tables from non-secure Linux

Result:
- Same physical DRAM can be partitioned.
- Secure world mappings can target secure-only regions inaccessible to non-secure world.

---

## 3. Linux and Firmware Implementation View

### 3.1 TF-A and OP-TEE flow

Typical boot model:
- BL31 (EL3) initializes security state handling.
- BL32 (OP-TEE) initializes secure EL1 page tables.
- BL33 boots Linux in non-secure EL2 or EL1.

Linux perspective:
- Runs entirely in non-secure world.
- Cannot directly map or access secure-only regions, even with EL1 privileges.
- Communicates with secure services through SMC calls.

### 3.2 Memory layout coordination

Firmware and device tree coordinate memory carveouts:
- secure reserved ranges excluded from Linux allocators
- shared memory windows defined for normal-secure communication

Common Linux-visible nodes:
- optee shared memory regions
- reserved-memory with no-map for secure-only carveouts

### 3.3 Virtualization corner cases

In virtualized systems, host and guest are normally non-secure unless platform supports extended security models.
Security partition still relies on EL3 policy and secure firmware ownership.

---

## 4. Hardware-Software Interaction

Secure service call example:
1. Linux at non-secure EL1 prepares shared buffer.
2. Executes SMC into EL3 monitor.
3. Monitor switches to secure context and dispatches into BL32.
4. OP-TEE processes request using secure page tables.
5. Returns result through shared memory to Linux.

Attempted non-secure access to secure memory:
1. Linux maps physical range believed to be available.
2. Access issued as non-secure transaction.
3. Fabric/TZASC denies access due to secure region policy.
4. Linux observes abort or bus error.

---

## 5. Interview Q and A

Q1: Is EL2 automatically secure?
No. EL and security state are independent dimensions. EL2 can run non-secure in common deployments.

Q2: Can Linux EL1 read secure DRAM if it maps the physical address?
No. Non-secure transaction attributes and downstream policy blocks secure-only regions.

Q3: What decides secure versus non-secure state at handoff?
EL3 firmware programs SCR_EL3 and context before ERET to next stage.

Q4: Why do we still need secure carveouts if Linux is non-secure?
Because allocators and drivers must avoid using ranges reserved for secure payloads and key material.

Q5: How does OP-TEE maintain isolation?
By running in secure world with separate translation context and by relying on interconnect memory security enforcement.

Q6: Where does Linux interact with secure world?
Through SMC-based interfaces, often standardized via PSCI and OP-TEE driver protocols.

---

## 6. Pitfalls and Gotchas

- Confusing privilege with security world leads to wrong threat models.
- Assuming page-table mapping alone grants access ignores fabric enforcement.
- Forgetting shared-memory carveouts breaks secure service communication.
- Incorrectly sized secure carveouts can overlap normal allocator pools.
- Documentation may vary by ARM architecture version; always check feature-specific manuals.

---

## 7. Quick Reference Table

| Concept | Purpose |
|---|---|
| EL0 to EL3 | Privilege hierarchy |
| Secure versus Non-secure | World partition hierarchy |
| SCR_EL3.NS | EL3 control for non-secure handoff |
| Secure TTBR context | Separate secure world address space |
| SMC | Controlled crossing between worlds |

| Component | Enforces what |
|---|---|
| EL3 monitor | World switch and context control |
| MMU tables | Address translation within a world |
| TZASC or fabric policy | Final secure-memory transaction allow or deny |
