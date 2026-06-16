# Intel TDX (Trusted Domain Extension) Deep Dive

Category: Advanced and Cutting-Edge Topics  
Platform: ARM64 (AArch64), Intel TDX architecture overview

---

## 1. Concept Foundation

Intel TDX is Intel's confidential computing approach for x86.

(Note: ARM focus, but TDX important for cross-platform understanding)

Key concepts:
- Trusted Domain (TD): isolated VM
- TD-exits: traps from guest to host
- TDMRs: memory regions for TDs

---

## 2. Intel TDX Hardware Detail

### 2.1 Memory encryption

Pages within TD memory automatically encrypted.
Host cannot read TD memory; only TD vCPU can read.

### 2.2 Attestation

TD can request attestation report.
Report signed by Intel, proving TD identity and measurements.

---

## 3. Linux Kernel Implementation (x86)

### 3.1 Guest side

Guest kernel detects TDX capability.
Uses different hypercall interface (TDVMCALL vs VMCALL).

### 3.2 Host hypervisor

QEMU/KVM extended for TD support.
Manages TD lifecycle and I/O emulation.

---

## 4. Comparison with ARM CCA

Similarities:
- isolation of guest memory
- attestation and measurement
- host manages I/O and resources

Differences:
- TDX: x86-specific, single-vCPU model
- CCA: ARM-native, multi-vCPU realms

---

## 5. Interview Q and A

Q1: How does TDX compare to CCA?
TDX: x86 approach, full VM model. CCA: ARM approach, realm model with guest kernel.

Q2: What is the threat model of TDX?
Protects against host kernel compromise (host can't read TD memory).

Q3: Can TDX measure software inside TD?
Yes, TD can compute attestation report with current state.

Q4: What is performance overhead of TDX?
Moderate; extra hypercalls, memory encryption, but similar to CCA.

Q5: Can TDX and CCA interoperate?
No; different architectures and threat models.

Q6: What is future of Intel and ARM confidential computing?
Industry convergence on confidential VM semantics (though architecture-specific implementations differ).

---

## 6. Pitfalls and Gotchas

- Assuming TDX and CCA are compatible (they're not).
- Confusing TDX with SEV (AMD's approach; similar but different semantics).
- Thinking confidential VMs eliminate host trust (still need secure host OS).

---

## 7. TDX Quick Reference Table

| Component | Purpose |
|---|---|
| TD | trusted domain (isolated VM) |
| TDMR | TD memory region |
| TDVMCALL | hypercall from TD to host |
| TDREPORT | attestation data from TD |
