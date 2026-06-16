# AMD SEV-SNP (Secure Encrypted Virtualization Secure Nested Paging) Deep Dive

Category: Advanced and Cutting-Edge Topics  
Platform: ARM64 context, AMD SEV-SNP overview for cross-platform understanding

---

## 1. Concept Foundation

AMD SEV-SNP extends SEV with additional protections.

Key features:
- memory encryption (like SEV)
- page state tracking
- replay attack prevention
- simplified attestation

---

## 2. AMD SEV-SNP Hardware Detail

### 2.1 Memory encryption

VM memory encrypted with VM-specific key.
Host cannot access VM memory.

### 2.2 Secure Nested Paging

Page table state tracked; prevents replay attacks.
Host cannot re-inject old page states.

### 2.3 RMP (Reverse Map Table)

Tracks ownership of each 4K page.
Prevents host from assigning page to wrong VM.

---

## 3. Implementation Patterns

### 3.1 Guest side

Guest detects SEV-SNP capability.
Cooperates with hypervisor for memory management.

### 3.2 Hypervisor side

QEMU/KVM implements SNP protocol.
Manages VM memory transitions and attestation.

---

## 4. Cross-Architecture Comparison

| Feature | SEV-SNP | CCA | TDX |
|---|---|---|---|
| encryption | yes | yes | yes |
| attestation | yes | yes | yes |
| replay protection | yes | implicit | implicit |
| multi-vCPU | yes | yes | yes |

---

## 5. Interview Q and A

Q1: What does SNP add to original SEV?
Replay attack prevention; tracks page state in hardware RMP.

Q2: How does SEV-SNP compare to CCA?
Both provide similar isolation; AMD x86 approach vs ARM approach.

Q3: What is the performance impact of SNP?
Moderate; mostly from page state management overhead.

Q4: Can you migrate SEV-SNP VM between hosts?
Difficult; VM state is tied to source host keys.

Q5: How does SEV-SNP attestation work?
Guest generates report, signed by AMD; verifier checks report signature.

Q6: What is GHCB (Guest Hypervisor Communication Block)?
Shared memory region for SEV guest-host communication.

---

## 6. Pitfalls and Gotchas

- Assuming SEV-SNP is perfectly secure (side-channels still possible).
- Confusing SEV, SEV-ES, and SEV-SNP (each adds features).
- Thinking AMD and Intel implementations are interchangeable (they're not).

---

## 7. SEV-SNP Quick Reference

| Component | Purpose |
|---|---|
| RMP | reverse map table for page tracking |
| GHCB | guest-hypervisor communication |
| VMSA | VM save area (vCPU state) |
| attestation report | cryptographic proof of VM identity |
