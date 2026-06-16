# OP-TEE Trusted Execution on ARM64 Deep Dive

Category: Security Extensions  
Platform: ARM64 (AArch64), OP-TEE as BL32

---

## 1. Concept Foundation

OP-TEE is a Trusted Execution Environment OS typically running as BL32 in secure world.

Purpose:
- Execute trusted applications isolated from normal-world Linux.
- Protect keys and sensitive operations.
- Provide standardized API model for secure services.

Core split:
- Normal world client app and kernel driver
- Secure world OP-TEE core and Trusted Applications

---

## 2. ARM64 Hardware Detail

Execution model:
- OP-TEE runs in secure state using secure translation context.
- Normal world requests service through SMC calls.
- EL3 monitor handles world switch and context transfer.

Memory model:
- Secure private memory for OP-TEE core and TA runtime state.
- Shared memory window for message passing with normal world.
- Access controls enforced by world state plus fabric policy.

Important context properties:
- Separate secure stacks and exception vectors
- Secure page tables independent from Linux mappings
- Controlled register save and restore on each world switch

---

## 3. Linux and OP-TEE Implementation

### 3.1 Components

Normal world:
- optee Linux driver
- tee subsystem device interfaces
- user-space libteec clients

Secure world:
- OP-TEE core scheduler and syscall handling
- pseudo TAs (built-in trusted services)
- user TAs loaded and managed by secure loader

### 3.2 Call flow

Typical request path:
1. User client opens session through TEE Client API.
2. Linux optee driver packages message in shared memory.
3. Driver issues SMC to secure monitor.
4. OP-TEE core dispatches to target TA command.
5. Result copied to shared memory and returned to client.

### 3.3 Paging and memory handling

OP-TEE may use pager support:
- Core can page parts of secure binaries based on demand.
- Secure heap and object allocators isolate TA data.
- Shared buffers are explicitly marked and validated to avoid pointer confusion across worlds.

---

## 4. Hardware-Software Interaction

Session example:
1. Linux app requests cryptographic sign operation.
2. optee driver allocates shared buffer and marshals parameters.
3. SMC enters secure world.
4. OP-TEE validates session and TA access policy.
5. TA reads key material from secure storage backend.
6. TA computes signature and writes result to shared memory.
7. Return path restores non-secure context and wakes client.

Security benefits:
- Key material never leaves secure world.
- Linux compromise does not automatically reveal trusted storage secrets.

---

## 5. Interview Q and A

Q1: Why use OP-TEE instead of kernel key APIs only?
OP-TEE keeps high-value keys and operations in secure world, reducing exposure if normal world kernel is compromised.

Q2: What is shared memory used for?
Parameter passing and bulk data exchange between normal and secure worlds.

Q3: Can Linux directly call a TA without SMC?
No. Crossing worlds requires secure monitor mediated transition.

Q4: What is a pseudo TA?
A built-in trusted service inside OP-TEE core, not loaded as separate user TA binary.

Q5: How is TA isolation achieved?
Through secure world memory management, process-like context separation, and controlled syscall interface inside OP-TEE.

Q6: What is a common performance bottleneck?
Frequent small SMC transitions and buffer marshaling overhead for chatty APIs.

---

## 6. Pitfalls and Gotchas

- Oversized shared buffers increase attack surface and validation burden.
- Incorrect cache maintenance around shared memory can corrupt data on some platforms.
- Mismatch between Linux driver ABI and OP-TEE version causes silent failures.
- Treating secure world as infinite trust can hide supply-chain and update risks.
- Debug prints from secure world may leak sensitive metadata if not controlled.

---

## 7. Quick Reference Table

| Component | Role |
|---|---|
| libteec client | User-space API to TEE services |
| optee Linux driver | Normal-world transport and SMC bridge |
| EL3 monitor | World switch mediation |
| OP-TEE core | Secure OS runtime and TA management |
| TA | Trusted application executing secure command logic |

| Memory type | Access scope |
|---|---|
| Secure private memory | OP-TEE core and TAs |
| Shared memory | Normal and secure worlds by explicit protocol |
