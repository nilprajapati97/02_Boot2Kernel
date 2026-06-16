# ARM64 Virtualization Memory: Stage 2 and Two-Stage Translation

**Category**: Virtualization Memory and Stage 2 Translation  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
ARM64 Virtualization: designed from the ground up for efficient VM support

Hardware exception levels:
  EL0: Userspace applications (lowest privilege)
  EL1: Guest OS kernel (inside VM) or bare-metal OS kernel
  EL2: Hypervisor (KVM, Xen, Hyper-V on ARM, seL4, pKVM)
  EL3: Secure Monitor (TF-A: Trusted Firmware-A)

Virtualization memory model:
  Without hypervisor (bare metal):
    CPU MMU: VA → PA (one stage, TTBR0/TTBR1 + TCR_EL1)
  
  With hypervisor (KVM/ARM64):
    Guest OS: VA → IPA (Stage 1 translation, guest's TTBR0/TTBR1)
    Hypervisor: IPA → PA (Stage 2 translation, VTTBR_EL2)
    Hardware: performs BOTH stages automatically per memory access
    
  IPA = Intermediate Physical Address (also called "guest physical address")
    From guest OS perspective: IPA looks like real physical memory
    Guest OS: programs page tables using IPA as "PA"
    Guest OS: does NOT know the real PA (hypervisor hides it)

Why two stages?
  Isolation: each VM gets its own IPA space (guest thinks it has 0..RAM_SIZE)
  Protection: hypervisor controls what real RAM each VM can access
  Consolidation: multiple VMs share one physical server
  Migration: hypervisor can remap IPA → new PA when migrating VM

ARM64 Stage 2 registers (at EL2):
  VTTBR_EL2: Stage 2 translation table base register
    [PA[47:1]] [VMID[63:48]] [CnP[0]]
    PA bits: physical address of Stage 2 L0 page table
    VMID: identifies which VM (affects IOTLB tagging)
  
  VTCR_EL2: Stage 2 translation control register
    T0SZ[5:0]: IPA size = 64 - T0SZ (e.g., T0SZ=24 → 40-bit IPA = 1TB)
    SL0[7:6]: start level (00=L2, 01=L1, 10=L0, 11=L3_only)
    IRGN0[9:8]: inner cacheability
    ORGN0[11:10]: outer cacheability
    SH0[13:12]: shareability
    TG0[15:14]: granule (00=4KB, 01=16KB, 10=64KB)
    PS[18:16]: physical address size (000=32bit, 001=36bit, ..., 101=48bit)
    VS[19]: VMID size (0=8bit, 1=16bit)
    NSW[29]: Stage 2 NS-Walk (non-secure table walk)
    NSA[30]: Stage 2 NS-Access (access to non-secure PA)
    DS[32]: 52-bit PA support (FEAT_LPA)

HCR_EL2 (Hypervisor Configuration Register):
  VM[0]: 1 = enable Stage 2 translation
  RW[31]: 1 = guest runs in AArch64, 0 = AArch32
  SWIO[1]: set WI (WFI/WFE intercept)
  TWI[13]: trap WFI to EL2
  IMO[4]: intercept physical IRQs to EL2
  FMO[3]: intercept physical FIQs to EL2
  TGE[27]: trap general exceptions to EL2 (VHE mode)
  E2H[34]: EL2+Host mode (VHE: same binary for bare-metal + hypervisor)
```

---

## 2. Stage 2 Translation Hardware Detail

```
Stage 2 page table format (4KB granule, 48-bit PA):
  
  Same as Stage 1 long-descriptor format, with minor differences:
  
  Level 0 table (VTTBR_EL2 points here):
    512 entries × 8 bytes = 4KB
    Each entry covers: 512GB IPA range
    Entry: [next_table_PA[47:12] | valid=1 | type=table=1]
  
  Level 1 table:
    512 entries × 8 bytes = 4KB
    Each entry covers: 1GB IPA range
    Block entry (1GB huge page):
      [PA[47:30] | AF | SH[9:8] | S2AP[7:6] | MemAttr[5:2] | valid=1 | type=0]
    Table entry: → L2 table
  
  Level 2 table:
    512 entries × 8 bytes = 4KB
    Each entry covers: 2MB IPA range
    Block entry (2MB huge page):
      [PA[47:21] | AF | SH | S2AP | MemAttr | valid=1 | type=0]
    Table entry: → L3 table
  
  Level 3 table:
    512 entries × 8 bytes = 4KB
    Each entry covers: 4KB IPA range
    Page entry:
      [PA[47:12] | AF | SH[9:8] | S2AP[7:6] | MemAttr[5:2] | valid=1 | type=3]

S2AP (Stage 2 Access Permissions):
  S2AP[1:0]:
    00: No access (all accesses fault)
    01: Read only
    10: Write only (reserved in practice)
    11: Read-write

MemAttr[3:0] in Stage 2 entry:
  Different encoding than Stage 1 (no MAIR index):
  [3:2] Outer attributes:
    00: Device memory
    01: Normal Non-Cacheable
    10: Normal WB RA WA (write-back, read-allocate, write-allocate)
    11: Normal WT RA (write-through, read-allocate)
  [1:0] Inner attributes: same encoding as [3:2]
  
  Full encoding for Device nGnRnE: 0b0000
  Full encoding for Normal WB WA: 0b1111

Access Flag (AF) in Stage 2:
  Same as Stage 1: must be set or access-flag fault
  With FEAT_HAFDBS: HW sets AF automatically

VA → IPA → PA walk hardware example:
  Guest accesses VA = 0x400000400000

  Stage 1 walk (in guest's EL1 page tables, using TTBR0_EL1):
    VA[47:39] = 0x02 → PGD[2] → L1 table PA
    VA[38:30] = 0x00 → PMD/PUD[0] → L2 table PA
    VA[29:21] = 0x00 → PTE_dir[0] → L3 table PA
    VA[20:12] = 0x00 → L3[0] = IPA[47:12] | AP | AF | valid
    IPA = 0x200000 | offset = IPA = 0x200000400
  
  BUT: every page table pointer read during Stage 1 walk is ALSO
       translated by Stage 2! (Nested walk penalty)
  
  Stage 2 walk (VTTBR_EL2, hypervisor's tables):
    IPA = 0x200000400
    IPA[47:39] → S2_L0[0]
    IPA[38:30] → S2_L1[1]
    IPA[29:21] → S2_L2[0]
    IPA[20:12] → S2_L3[0] = PA[47:12] | S2AP=11 | MemAttr=1111 | AF | valid
    PA = actual physical DRAM address
  
  Performance: S1+S2 combined = up to 24 page table reads (vs 4 for S1 only!)
  ARM64 TLB: caches combined VA→PA result (software sees final PA directly)
  TLB miss: triggers hardware walker (full 2-stage walk is automatic)
```

---

## 3. VMID (VM Identifier)

```
VMID: Virtual Machine Identifier
  Tags TLB entries to distinguish translations from different VMs
  Analogous to ASID (tags TLB for different processes within one VM)
  
  Without VMID: switching between VMs requires TLBI VMALLE1IS
    Expensive: flush all TLB entries on every VM switch
  
  With VMID: different VMs have different VMIDs
    TLB entries tagged: {ASID, VMID, VA} → PA
    VM switch: change VTTBR_EL2 (new VMID, new Stage 2 tables)
    Old VM's TLB entries: remain cached (different VMID = no conflict)
    New VM TLB warm-up: just start executing → TLB fills on demand
  
  VMID size:
    VTCR_EL2.VS = 0: 8-bit VMID (256 VMs simultaneously in TLB)
    VTCR_EL2.VS = 1: 16-bit VMID (65536 VMs simultaneously in TLB)
    ARM64 Neoverse N1/N2: support 16-bit VMID
    Linux KVM: uses 16-bit VMID when VTCR_EL2.VS=1 available
  
  VMID allocation in Linux KVM:
    kvm_vmid_init(): create kvm_vmid_ns (namespace)
    kvm_create_vcpu(): kvm_vmid_alloc()
    If VMID exhausted: kvm_vmid_bump_generation()
      → All VMs get VMID 0 temporarily
      → On next vcpu_run(): reassign VMIDs from new generation
      → kvm_flush_remote_tlbs(): flush all TLB entries (TLBI VMALLS12E1IS)
    
    Lazy VMID rollover:
      Don't flush TLB immediately when VMID wraps
      Just let VMs run with new VMIDs
      Old TLB entries with old generation VMID: can't match (different VMID bits)
      Effectively: stale entries never accessed again

VMID in ARM64 TLBI instructions:
  TLBI VMALLE1: invalidate all Stage 1 entries for current VMID
  TLBI VMALLS12E1: invalidate all Stage 1+2 entries for current VMID
  TLBI IPAS2E1IS: invalidate Stage 2 entries by IPA (inner shareable)
  TLBI IPAS2LE1IS: invalidate Stage 2 leaf entries by IPA
  TLBI ALLE2: invalidate all EL2 TLB entries
```

---

## 4. Interview Questions & Answers

**Q1: Explain the two-stage memory translation in ARM64 virtualization. What is each stage responsible for?**

ARM64 virtualization uses two hardware translation stages:

**Stage 1 (VA → IPA)**: Controlled by the guest OS (EL1). The guest programs `TTBR0_EL1`/`TTBR1_EL1` with page table bases, just like a bare-metal OS. The Stage 1 table maps guest virtual addresses to "Intermediate Physical Addresses" (IPA) — what the guest thinks are physical addresses. The guest has no knowledge of Stage 2 or real physical addresses.

**Stage 2 (IPA → PA)**: Controlled by the hypervisor (EL2). The hypervisor programs `VTTBR_EL2` with the Stage 2 page table. It maps IPA → real PA (host physical memory). The hypervisor controls which physical pages each VM can access. The guest cannot change Stage 2 (requires EL2 privilege).

**Combined hardware walk**: ARM64 hardware performs both walks automatically for every memory access. `HCR_EL2.VM=1` enables Stage 2. On TLB miss: hardware walker reads Stage 1 page tables (each pointer also goes through Stage 2!) then reads Stage 2 page tables. The final result (VA → PA) is cached in the TLB as a single entry.

**Performance impact**: a 4KB page combined walk can require up to 4 S1 reads × 4 S2 reads (each S1 pointer also needs S2 translation) = up to 24 memory reads vs 4 for bare metal. Hardware TLB caching and hardware page table walker make this transparent to software.

**Q2: What happens to the TLB when the hypervisor switches between two VMs?**

When KVM switches from VM-A to VM-B:
1. KVM loads `VTTBR_EL2` for VM-B (new VMID, new Stage 2 page table base)
2. CPU sees new VMID in VTTBR_EL2
3. TLB lookups now only match entries tagged with VM-B's VMID
4. VM-A's TLB entries: still present but VMID doesn't match → never used
5. VM-B starts running: initial TLB misses, hardware fills from VM-B's Stage 1+2 tables
6. No explicit TLB flush required (VMID isolation provides free switch)

If VMIDs wrap around (too many VMs for VMID bits): `kvm_vmid_bump_generation()` increments the generation counter. On next VM entry: each VM gets a new VMID. At that point, a full TLBI is issued (`TLBI VMALLS12E1IS`) to clear all stale entries from the old generation. This is called "VMID rollover".

---

## 5. Quick Reference

| Register | Purpose |
|---|---|
| VTTBR_EL2 | Stage 2 page table base + VMID |
| VTCR_EL2 | Stage 2 control (size, granule, cacheability) |
| HCR_EL2.VM | Enable Stage 2 translation |
| HCR_EL2.RW | Guest AArch64 (1) or AArch32 (0) |
| TTBR0_EL1 | Guest Stage 1 (EL0 VA space) |
| TTBR1_EL1 | Guest Stage 1 (EL1 VA space) |

| Stage 2 Entry Type | IPA Coverage | Level |
|---|---|---|
| 1GB block | 1GB per entry | Level 1 |
| 2MB block | 2MB per entry | Level 2 |
| 4KB page | 4KB per entry | Level 3 |

| TLBI for Stage 2 | What It Invalidates |
|---|---|
| TLBI VMALLE1IS | All Stage 1 for current VMID |
| TLBI VMALLS12E1IS | All Stage 1+2 for current VMID |
| TLBI IPAS2E1IS | Stage 2 by IPA range |
| TLBI ALLE2IS | All EL2 entries |
