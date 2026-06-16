# TF-A Boot Flow on ARM64 — BL1, BL2, BL31, BL32, BL33

Category: Security Extensions  
Platform: ARM64 (AArch64), Trusted Firmware-A ecosystem

---

## 1. Concept Foundation

Trusted Firmware-A (TF-A) establishes the root of trust and secure boot handoff chain before Linux starts.

Boot stage model:
- BL1: immutable first-stage in ROM or ROM-like storage
- BL2: trusted bootloader stage that loads and verifies next images
- BL31: EL3 runtime firmware and secure monitor
- BL32: secure payload, often OP-TEE
- BL33: non-secure world payload, usually UEFI then Linux

Design goals:
- Verify authenticity and integrity of each stage.
- Initialize security-critical registers and memory attribution.
- Enforce secure and non-secure world separation before handing off.

---

## 2. ARM64 Hardware Detail

### 2.1 EL3 controls used by TF-A

Critical EL3 registers:
- SCR_EL3: controls non-secure state, routing, execution state controls
- SPSR_EL3: defines target EL and PSTATE for ERET handoff
- ELR_EL3: return address for handoff target
- VBAR_EL3: EL3 exception vector base
- SCTLR_EL3: EL3 MMU and cache behavior

Typical handoff pattern:
1. BL31 prepares target context.
2. Programs ELR_EL3 with BL33 entry point (or BL32 entry first).
3. Programs SPSR_EL3 to requested next EL (EL2 or EL1) and masks.
4. Executes ERET to transfer control.

### 2.2 Secure state transitions

TrustZone state is encoded through secure context and SCR_EL3 controls.
- Non-secure handoff sets NS state for normal world payload.
- Secure payload entry preserves secure context.

Memory controllers and firewalls (for example TZC/TZASC) are configured so secure DRAM ranges are inaccessible from non-secure transactions.

---

## 3. Linux and Firmware Implementation View

### 3.1 Stage responsibilities

BL1:
- Performs minimal initialization.
- Authenticates BL2 from trusted source.
- Transfers to BL2 in secure state.

BL2:
- Loads BL31, BL32, BL33 from FIP or platform media.
- Verifies signatures/hashes under chain-of-trust policy.
- Builds handoff descriptors (entry point info, memory params).

BL31:
- Initializes EL3 runtime services and SMC handler.
- Sets up secure monitor call interface (PSCI, optional vendor services).
- Context-manages world switches between normal and secure worlds.

BL32 (optional but common):
- OP-TEE or other trusted OS runtime.
- Handles trusted applications and secure services.

BL33:
- Usually UEFI firmware then Linux kernel Image.
- Runs in non-secure EL2 or EL1 based on platform policy.

### 3.2 Data structures and handoff blocks

Common handoff objects in TF-A platform code include:
- entry_point_info_t
- image_info_t
- bl_params_t

These carry:
- image load addresses
- execution state
- security state
- argument registers passed to next stage

### 3.3 TBBR and authentication model

Trusted Board Boot Requirements model includes:
- Root of trust public key anchored in immutable storage
- Certificate chain and image signature validation
- Anti-rollback/version policy on supported platforms

---

## 4. Hardware-Software Interaction

Typical execution timeline:
1. ROM executes BL1 from immutable memory.
2. BL1 authenticates and loads BL2.
3. BL2 authenticates and loads BL31, BL32, BL33.
4. BL31 starts at EL3, sets up monitor and runtime services.
5. BL31 optionally enters BL32 secure payload initialization.
6. BL31 hands off to BL33 non-secure payload via ERET.
7. BL33 boots UEFI or directly Linux.
8. Linux later issues SMC calls to PSCI through EL3 monitor.

Runtime interaction example:
- Linux CPU hotplug or suspend invokes PSCI SMC.
- EL3 monitor validates and routes secure operation.
- Control returns to Linux with status code.

---

## 5. Interview Q and A

Q1: Why is BL31 special compared to BL2?
BL31 stays resident at EL3 as runtime firmware and secure monitor, while BL2 is generally a load-and-handoff stage.

Q2: What decides whether Linux starts at EL2 or EL1?
Platform and firmware policy in BL31 and boot chain configuration determine target EL in SPSR_EL3 and handoff settings.

Q3: Where is secure versus non-secure split enforced?
By EL3 state controls, secure monitor context management, and hardware firewalls that tag memory and peripherals by security domain.

Q4: What is the role of FIP?
Firmware Image Package bundles signed boot images and metadata for authenticated loading by BL2.

Q5: How does Linux use TF-A after boot?
Through SMC calls, mainly PSCI for power management and CPU control services.

Q6: What breaks if BL2 verification is bypassed?
Chain of trust collapses; untrusted BL31 or BL33 code can run with high privilege, defeating secure boot guarantees.

---

## 6. Pitfalls and Gotchas

- Misconfigured SPSR_EL3 can hand off in wrong EL or state and fail early boot.
- Incorrect secure memory firewall setup can expose secure payload memory to normal world.
- Incomplete certificate chain handling causes boot fragility across update cycles.
- Mixing incompatible BL31 and BL32 builds leads to SMC ABI failures.
- Poor anti-rollback policy allows downgrading to vulnerable firmware.

---

## 7. Quick Reference Table

| Stage | Typical EL | Core role |
|---|---|---|
| BL1 | EL3 secure | Immutable first trust anchor and BL2 authentication |
| BL2 | EL3 secure | Load and verify later images |
| BL31 | EL3 secure runtime | Secure monitor and SMC services |
| BL32 | S-EL1 or secure runtime model | Trusted OS such as OP-TEE |
| BL33 | NS-EL2 or NS-EL1 | UEFI and Linux boot payload |

| Key register | Why it matters |
|---|---|
| SCR_EL3 | Secure and non-secure routing and control |
| SPSR_EL3 | Defines target context for ERET handoff |
| ELR_EL3 | Target entry address for next stage |
| VBAR_EL3 | EL3 exception vectors |
