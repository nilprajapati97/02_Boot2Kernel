# MAIR_EL1 in Hypervisor and Secure World Contexts

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm

---

## 1. MAIR at Different Exception Levels

```
ARM64 has separate MAIR registers per exception level:

  MAIR_EL1: Used for EL0/EL1 translations (user + kernel)
            Linux kernel sets and uses this exclusively
            
  MAIR_EL2: Used for EL2 Stage 1 translations (hypervisor's own data)
            KVM host uses this when running at EL2 (VHE mode)
            
  MAIR_EL3: Used for EL3 translations (ARM Trusted Firmware / Secure Monitor)
            ATF sets this for secure monitor data structures

  MAIR_EL1 (from EL2 in non-VHE mode): The GUEST OS's MAIR_EL1
    When a guest OS runs at EL1, its MSR MAIR_EL1 accesses this register
    The hypervisor (EL2) has its own separate MAIR_EL2

Each exception level maintains complete independence of memory attribute config.
A guest OS can set MAIR_EL1 to any value; it only affects the guest's translations.
```

---

## 2. MAIR_EL2 in KVM (Virtualization Host Extensions, VHE)

```
VHE (Virtualization Host Extensions, ARMv8.1):
  Host Linux kernel runs at EL2 (instead of EL1 as in non-VHE)
  SCTLR_EL2, TCR_EL2, TTBR0_EL2, MAIR_EL2 replace EL1 counterparts for host

VHE MAIR_EL2 setup (arch/arm64/kvm/hyp/nvhe/setup.S or VHE path):
  // Host kernel (at EL2 in VHE) uses MAIR_EL2 for its own pages:
  ldr x5, =MAIR_EL1_SET     // Same values as MAIR_EL1
  msr MAIR_EL2, x5           // Host kernel's memory types
  
  // When running a guest (EL1) OS:
  // Guest's MAIR_EL1 register value is saved/restored on VM entry/exit
  // Host MAIR_EL2 is unaffected by guest MAIR_EL1 writes

  // KVM guest memory type override (Stage 2):
  // Stage 2 descriptors have MemAttr field (NOT MAIR-indexed)
  // Stage 2 MemAttr directly encodes inner/outer attrs without MAIR lookup
  // More on Stage 2 MemAttr encoding: see Category 13 (Virtualization)
```

---

## 3. Combined Memory Attributes in Stage 1 + Stage 2

```
When both Stage 1 (guest MAIR_EL1) and Stage 2 (hypervisor) are active:

Attribute Combining Rules (ARM DDI 0487 Chapter D8):
  Final attribute = intersection(Stage 1 attr, Stage 2 attr)
  "Intersection" = MORE RESTRICTIVE attribute wins

Device vs Normal:
  If either stage says Device → final is Device
  Both must say Normal for result to be Normal

Cacheability:
  If Stage 1 says WB and Stage 2 says NC → result is NC
  If Stage 1 says NC and Stage 2 says WB → result is NC
  Both must say WB for result to be WB

Practical impact:
  Guest OS maps page as Normal WB (guest doesn't know it's a device):
    Stage 2 (KVM hypervisor) maps same GPA as Device nGnRnE
    Final: Device nGnRnE (Stage 2 overrides Stage 1)
    → Correct! Guest accidentally mapped device as Normal, hypervisor corrects it

  Guest maps RAM as Normal WB:
    Stage 2 maps as Normal WB
    Final: Normal WB → cached access ✓

  Hypervisor maps guest RAM as NC for isolation:
    Stage 1 (guest): Normal WB
    Stage 2 (hypervisor): NC
    Final: NC → guest's cache accesses become uncached
    Use case: hypervisor may mark certain secure memory as NC to prevent
    guest from caching sensitive hypervisor data via speculative accesses

ASCII: Stage 1 + Stage 2 combined:
  Guest PTE → Stage 1 attribute (from MAIR_EL1[AttrIndx])
  KVM Stage 2 PTE → Stage 2 attribute (embedded in descriptor)
  Hardware: uses min(Stage1, Stage2) for final access
```

---

## 4. MAIR_EL3 in ARM Trusted Firmware (ATF/TF-A)

```
ARM Trusted Firmware sets MAIR_EL3 for the secure monitor (BL31):

Typical ATF MAIR_EL3 configuration:
  Slot 0: MT_DEVICE = 0x00 (Device nGnRnE for secure device registers)
  Slot 1: MT_NORMAL = 0xFF (Normal WB for secure RAM)
  Slot 2: MT_RW_DATA = 0xFF (read-write data, same as Normal WB)
  Slot 3: MT_CODE = 0xFF (code, same Normal WB but PXN=0)

ATF page table creation:
  xlat_table_init() sets up EL3 page tables
  Memory type macros: MT_DEVICE, MT_MEMORY (WB), MT_NON_CACHEABLE (NC)
  ATF's mm_region_t.attr includes MT_xxx flags → mapped to MAIR slots

Secure/Non-Secure NS bit:
  In secure world (EL3): NS bit in page table descriptor selects
  secure vs non-secure PA space
  MAIR_EL3 applies to all accesses, secure and non-secure, in EL3
```

---

## 5. Guest MAIR Virtualization

```
KVM must handle guest MAIR_EL1 access:

In non-VHE mode (host at EL2, guest at EL1):
  Guest at EL1 writes: MSR MAIR_EL1, X0
  Hardware: writes to MAIR_EL1 (real register at EL1)
  KVM does NOT trap this (MAIR_EL1 is not an HCR_EL2 trap target)
  Guest has full ownership of MAIR_EL1

  At VM entry (entering guest):
    KVM loads guest's saved MAIR_EL1 → hardware MAIR_EL1
  At VM exit (returning to host):
    KVM saves hardware MAIR_EL1 → guest's vcpu context
    KVM restores host's MAIR_EL1 (if non-VHE, host uses EL1 for syscalls)

In VHE mode (host at EL2):
  Host uses MAIR_EL2 for its own pages
  Guest (EL1) writes MAIR_EL1 → goes to physical MAIR_EL1 (separate from EL2)
  No save/restore for MAIR_EL1 needed for host
  Only guest's own MAIR_EL1 must be saved on VCPU context switch between guests

Performance optimization:
  MAIR_EL1 is only restored on guest vCPU context switch (between different VMs)
  Within the same VM, MAIR_EL1 doesn't need to be saved/restored
  (A single VM's guest OS keeps its MAIR_EL1 constant throughout operation)
```

---

## 6. TrustZone: Secure vs Non-Secure Memory Attributes

```
TrustZone partitions memory into Secure and Non-Secure worlds.
Memory attributes interact with this partitioning:

NS bit in page table descriptor (bit[5] for Stage 1 at EL1/EL0):
  NS=0: Output address is in Secure PA space (only accessible from Secure world)
  NS=1: Output address is in Non-Secure PA space (accessible by Normal world)

Memory attribute ordering with TrustZone:
  Secure world (S-EL1) accesses Secure PA:
    Uses MAIR_EL1 (secure world's EL1) for attribute lookup
    Same encoding as normal MAIR_EL1 but for secure memory

  Non-Secure world (NS-EL1 = Linux) accesses NS PA:
    Uses Linux's MAIR_EL1 for attribute lookup

  Secure world accessing Non-Secure PA (NS=1):
    Uses secure MAIR_EL1 with NS=1 output
    Memory type applies to the NS physical address

  TZASC (TrustZone Address Space Controller):
    Hardware firewall on DRAM bus
    Enforces which PA ranges are Secure-only vs NS
    Even if NS world creates PTE with NS=0 → TZASC blocks the access
    Memory type in MAIR is irrelevant if TZASC denies the transaction
```

---

## 7. Interview Questions & Answers

**Q1: When a KVM guest sets MAIR_EL1 to configure Device nGnRnE in slot 0, and the guest maps a GPA with AttrIndx=0, what is the final memory type if KVM's Stage 2 maps that GPA as Normal WB?**

The final memory type is **Device nGnRnE** (the more restrictive type). ARM's combining rules say: if either Stage 1 or Stage 2 specifies Device memory, the combined type is Device. Normal WB (Stage 2) + Device nGnRnE (Stage 1) → Device nGnRnE. This means even though the hypervisor mapped the GPA as Normal WB (perhaps intending it as RAM), the guest's Stage 1 Device attribute forces Device semantics on the combined access. In practice, KVM carefully constructs Stage 2 mappings to match the guest's intended memory types — typically Device for MMIO passthrough GPA ranges and Normal WB for RAM GPA ranges. If there's a mismatch (guest thinks RAM, hypervisor says Device), performance suffers but operation is still "correct" in terms of ordering guarantees.

---

## 8. Quick Reference

| Register | Exception level | Used by |
|---|---|---|
| MAIR_EL1 | EL0/EL1 | Linux kernel, guest OS |
| MAIR_EL2 | EL2 | Hypervisor (KVM VHE), Xen |
| MAIR_EL3 | EL3 | ARM Trusted Firmware (BL31) |

| Stage | Attribute source | Encoding |
|---|---|---|
| Stage 1 (EL1) | MAIR_EL1 slot lookup via AttrIndx | 8-bit encoding in register |
| Stage 2 (EL2) | Direct in Stage 2 descriptor MemAttr | 4-bit inner+outer nibbles |
| Combined | min(Stage1, Stage2) | More restrictive wins |

| Combined result rule | Outcome |
|---|---|
| Either says Device | Device |
| Both say Normal, one says NC | Normal NC |
| Both say Normal WB | Normal WB |
| Stage 1 caching stricter | Stage 1 wins |
| Stage 2 caching stricter | Stage 2 wins |
