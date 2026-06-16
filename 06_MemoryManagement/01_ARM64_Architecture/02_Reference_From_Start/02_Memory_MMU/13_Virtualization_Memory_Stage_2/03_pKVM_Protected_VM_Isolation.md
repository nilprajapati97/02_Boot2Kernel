# pKVM: Protected KVM and Confidential Computing on ARM64

**Category**: Virtualization Memory and Stage 2 Translation  
**Platform**: ARM64 (AArch64)

---

## 1. pKVM Concept and Motivation

```
Traditional KVM security model:
  Host Linux kernel: full control of hypervisor
  Problem: if host Linux is compromised (root exploit, kernel vulnerability):
    Attacker can read ALL VM memory (no protection from privileged host)
    Attacker can modify Stage 2 page tables → map VM memory into host
    Cloud tenant: must trust cloud provider's kernel (large attack surface)

pKVM (protected KVM):
  Goal: VMs protected from a compromised host Linux kernel
  
  Key insight: separate the MINIMAL hypervisor (EL2) from the large, untrusted
               host kernel (EL1). The EL2 code is small, formally verifiable,
               and does NOT trust the host kernel.

pKVM threat model:
  TRUSTED:     Guest VMs (they own their memory)
               pKVM EL2 hypervisor (small, verified)
  UNTRUSTED:   Host Linux kernel (may be compromised)
               Host userspace (may be malicious)
               Devices without SMMU (cannot DMA to protected memory)

Comparison:
  Traditional KVM:    Host kernel → fully trusted → can read VM memory
  pKVM:               Host kernel → explicitly excluded from VM memory
  
  ARM CCA (Realm):    Hardware-enforced: RMM at EL3 → even stronger guarantees
  Intel TDX:          Similar goal on x86 (Trust Domain Extensions)
  AMD SEV-SNP:        Similar goal on x86 (Secure Encrypted Virtualization)
```

---

## 2. pKVM Architecture

```
pKVM exception level separation:

EL3:  TF-A (Trusted Firmware-A) — ROM/signed code
      Boots system, hands off to EL2 pKVM hypervisor
      Provides secure/non-secure world separation
      
EL2:  pKVM hypervisor (small codebase, ~50K lines)
      Controls: Stage 2 page tables for ALL VMs AND for host Linux
      Controls: SMMU Stage 2 (device DMA isolation)
      Does NOT trust: anything from EL1 (host Linux)
      Validates: all hypercalls from host kernel
      Protected from: EL1 cannot modify EL2 page tables
      
EL1:  Host Linux kernel (normal kernel + KVM module)
      Can create VMs via hypercalls to pKVM EL2
      Cannot read protected VM memory (no S2 mappings into host for VM pages)
      
EL0:  Applications, QEMU, VMs (guest EL1 runs with Stage 2 active)

Memory ownership model:
  All physical memory: tracked by pKVM EL2 as "owned by X"
  Host memory: owned by host (EL2 maps into host Stage 2)
  VM memory: owned by VM (NOT mapped into host Stage 2)
  pKVM memory: owned by EL2 itself (EL1 cannot access)
  Shared memory: explicitly shared by host + VM (e.g., virtio rings)
  
  Page state transitions (like AMD SEV-SNP page states):
    Host private → Guest private: host calls HVC_SHARE_TO_GUEST
      pKVM: removes from host Stage 2, adds to guest Stage 2
    Guest private → Host private: guest calls HVC_UNSHARE
      pKVM: removes from guest Stage 2, zeros page, adds back to host Stage 2
    → Zeroing ensures host cannot snoop data left by guest!
```

---

## 3. pKVM Memory Isolation Implementation

```c
/* arch/arm64/kvm/hyp/nvhe/mem_protect.c */

/* pKVM page ownership table */
struct pkvm_page_state {
    u8 owner;           // PKVM_PAGE_OWNED_BY_HOST, GUEST, HYP, DEVICE
    u8 state;           // PKVM_PAGE_MAPPED, UNMAPPED, SHARED
};

/* One entry per physical page: */
struct pkvm_memory_map {
    struct pkvm_page_state *pages; // indexed by pfn
    phys_addr_t phys;
    size_t size;
    spinlock_t lock;
};

/* Host stage 2: maps host memory */
struct kvm_pgtable host_s2_pgt;

/* Guest stage 2: per-VM */
struct kvm_pgtable guest_s2_pgt; // in struct kvm_arch

Page donation flow (host → guest):
  kvm_pgtable_hyp_map(guest_domain, ipa, pa, size, prot):
    1. pkvm_page_state_lookup(pfn):
       Verify: page owned by host (state = OWNED_BY_HOST)
    2. host_unmap(pa, size):
       Remove from host Stage 2: kvm_pgtable_stage2_unmap(host_s2_pgt, pa, size)
       DSB + TLBI VMALLS12E1IS (host VMID)
    3. page_state_set(pfn, OWNED_BY_GUEST):
       Atomically update ownership table
    4. guest_map(ipa, pa, size, prot):
       Add to guest Stage 2: kvm_pgtable_stage2_map(guest_s2_pgt, ipa, pa, size, prot)
    5. Both: done atomically → at no point is page accessible by both

Page reclaim flow (guest → host):
  kvm_pgtable_hyp_unmap(guest_domain, ipa, size):
    1. guest_unmap(ipa, size):
       Remove from guest Stage 2
       DSB + TLBI IPAS2E1IS (guest VMID)
    2. memset(page, 0, PAGE_SIZE):
       ZERO the page (critical! prevents host snooping)
    3. page_state_set(pfn, OWNED_BY_HOST)
    4. host_map(pa, size):
       Re-add to host Stage 2
    
    IMPORTANT: step 2 (zeroing) ensures that even after host regains the page,
               host cannot read any guest data from it.

pKVM hypercalls (from host Linux to EL2 pKVM):
  HVC_HOST_MAP_HYPMEM: map some memory for EL2 (pKVM extended region)
  HVC_HOST_SHARE_MEM: share host page with guest (virtio)
  HVC_HOST_UNSHARE_MEM: unshare guest page, return to host
  HVC_CREATE_PRIVATE_MAPPING: allocate guest VA→IPA mapping
  HVC_INIT_VM: create VM (EL2 allocates VMID, Stage 2)
  HVC_VCPU_RUN: run a vCPU (EL2 enters guest)
```

---

## 4. Attestation and Measured Boot

```
pKVM measurement chain:
  Goal: tenant can cryptographically verify their VM is running
        on genuine pKVM and their code is unmodified

  1. TF-A (EL3): first stage of trust
     Signed by ARM or platform vendor
     Measures pKVM EL2 image: SHA256(pKVM_EL2_binary)
     Extends: PCR0 in TPM (or equivalent trusted storage)
  
  2. pKVM (EL2): measures guest VM:
     SHA256(guest_kernel + initrd + dtb)
     Provides: attestation report (signed by platform)
     
  3. Guest VM: receives attestation evidence
     Tenant: verifies attestation report against known-good values
     Only proceed if: pKVM is genuine AND guest was booted correctly
  
  ARM CCA (Realm) is the stronger version:
    Hardware: RMM (Realm Management Monitor) at EL3 certified by ARM
    Realm token: cryptographically signed measurement report
    Guest: can query its own measurement at runtime (ATTEST_REALM)

Practical use: Google Confidential GKE (Google Kubernetes Engine)
  Kubernetes pods run inside pKVM VMs
  Node operator (Google) cannot read pod memory
  Customer: verifies attestation before deploying secrets into pod
```

---

## 5. Interview Questions & Answers

**Q1: How does pKVM prevent the host Linux kernel from reading VM memory?**

pKVM enforces memory isolation through the Stage 2 page tables. The pKVM EL2 hypervisor (not the host kernel) controls the Stage 2 tables:

**For host Linux**: pKVM creates a Stage 2 page table for the host itself (`host_s2_pgt`). The host kernel also runs with Stage 2 enabled! This means: even the host kernel's physical memory accesses go through pKVM's Stage 2 table for validation.

**For VMs**: When memory is donated to a VM (host calls `HVC_HOST_SHARE_MEM`):
1. pKVM removes the page from the **host's Stage 2** (host can no longer access it)
2. pKVM adds the page to the **guest's Stage 2** (guest can access it)

At no point can both host and guest access the same page simultaneously. The host kernel cannot reverse this: it cannot write to its own Stage 2 table (requires EL2 privilege). The host can only request page reclamation via hypercall, and pKVM ZEROS the page before returning it to the host.

Even if the host kernel is fully compromised (root 0-day exploit), the attacker cannot read VM memory because: (1) the host's Stage 2 table doesn't map VM pages, and (2) the attacker is at EL1 — they cannot modify Stage 2 tables (requires EL2 privilege).

**Q2: What is the difference between pKVM and ARM CCA (Realm)?**

Both protect VMs from a compromised host. The key difference is the **trust anchor**:

**pKVM**:
- Trust anchor: pKVM EL2 code (software, can be audited by customers)
- pKVM code is in Linux kernel source (open source)
- Limitation: if EL2 pKVM itself is compromised (zero-day in EL2), all VMs are exposed
- Measurement: pKVM image measured by TF-A (EL3)
- Available: production in Android Virtualization Framework (AVF), experimental in upstream kernel

**ARM CCA (Realm Management Extension)**:
- Trust anchor: RMM (Realm Management Monitor) at EL3 (CPU hardware enforced)
- Realm VMs are protected by the RMM — NOT by EL2 hypervisor
- The hypervisor (KVM/Xen) at EL2 is also untrusted (like the host kernel)
- Memory: Realm memory is hardware-encrypted + access-controlled by RMM
- Attestation: hardware-signed (RoT = Root of Trust in silicon)
- EL3 enforcement: RMM validates all memory transitions between Normal/Realm worlds
- Available: ARMv9.2+ (2022+); Cortex-A725, Neoverse N3 etc.

Summary: pKVM trusts the EL2 hypervisor, protects from EL1 host. ARM CCA trusts only EL3 hardware, protects from both EL1 host AND EL2 hypervisor.

---

## 6. Quick Reference

| pKVM Memory State | Description |
|---|---|
| Host private | Mapped in host Stage 2 only |
| Guest private | Mapped in guest Stage 2 only (not in host S2) |
| Shared | Mapped in both (explicit sharing: virtio) |
| Hypervisor | Mapped in EL2 only (neither host nor guest S2) |

| pKVM vs Traditional KVM | Traditional | pKVM |
|---|---|---|
| Host can read VM memory | Yes (via Stage 2) | No (no S2 mapping) |
| Host controls Stage 2 | Yes (host kernel) | No (EL2 pKVM only) |
| Trust requirement | Trust host kernel | Trust only EL2 pKVM code |
| Attestation | None | pKVM image measurement |
| Performance overhead | None | ~1-5% for page transitions |
