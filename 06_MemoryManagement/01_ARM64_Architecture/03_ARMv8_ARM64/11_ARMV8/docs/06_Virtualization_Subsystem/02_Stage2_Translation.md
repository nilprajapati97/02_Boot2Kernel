# Stage-2 Address Translation

## 1. Two-Stage Translation

ARMv8 virtualization uses two stages of address translation for guest VMs:

```
Address Translation for a Guest:

  Guest Virtual Address (VA)          ← Guest application pointer
       │
       │ Stage-1 Translation          ← Guest OS page tables
       │ (controlled by guest EL1)       (TTBR0/1_EL1)
       ▼
  Intermediate Physical Address (IPA) ← What guest thinks is PA
       │
       │ Stage-2 Translation          ← Hypervisor page tables
       │ (controlled by EL2)             (VTTBR_EL2)
       ▼
  Physical Address (PA)               ← Real hardware address


  Example:
    Guest app accesses VA 0x0040_0000
    → Stage-1: Guest page table maps → IPA 0x8000_0000
    → Stage-2: Hypervisor maps      → PA  0x2_0000_0000
    → Actual DRAM access at 0x2_0000_0000

  Without virtualization (bare metal):
    VA → Stage-1 → PA (single stage, no IPA)
```

---

## 2. VTTBR_EL2 — Virtualization Translation Table Base

```
VTTBR_EL2 structure:
  ┌──────────────────────────────────────────────────────────────┐
  │ [63:48] VMID  — Virtual Machine Identifier                  │
  │                  (8 or 16 bits depending on VTCR_EL2.VS)    │
  │ [47:1]  BADDR — Base address of Stage-2 translation table   │
  │ [0]     CnP   — Common not Private (share TLB entries)     │
  └──────────────────────────────────────────────────────────────┘

  Each VM has its own VTTBR_EL2 value.
  On VM switch: hypervisor loads new VM's VTTBR_EL2.

VTCR_EL2 — Virtualization Translation Control Register:
  ┌─────────────────────────────────────────────────────────────┐
  │ T0SZ  [5:0]  — Size of IPA space (e.g., 16 → 48-bit IPA) │
  │ SL0   [7:6]  — Starting level of walk (0,1,2,3)           │
  │ IRGN0 [9:8]  — Inner cacheability                         │
  │ ORGN0 [11:10]— Outer cacheability                          │
  │ SH0   [13:12]— Shareability                                │
  │ TG0   [15:14]— Granule size (4KB/16KB/64KB)               │
  │ PS    [18:16]— Physical Address Size                       │
  │ VS    [19]   — VMID size: 0=8-bit, 1=16-bit               │
  └─────────────────────────────────────────────────────────────┘
```

---

## 3. Stage-2 Page Table Walk

```
IPA → PA translation (4KB granule, 40-bit IPA):

  IPA bits:
  ┌──────────┬──────────┬──────────┬──────────┬──────────┐
  │ [39:30]  │ [29:21]  │ [20:12]  │ [11:0]   │          │
  │ L1 index │ L2 index │ L3 index │ Offset   │          │
  │ (9 bits) │ (9 bits) │ (9 bits) │ (12 bits)│          │
  └─────┬────┴─────┬────┴─────┬────┴──────────┘          │
        │          │          │                            │
        ▼          ▼          ▼                            │
   ┌────────┐ ┌────────┐ ┌────────┐                      │
   │L1 Table│→│L2 Table│→│L3 Table│→ PA + Offset         │
   └────────┘ └────────┘ └────────┘                      │
    512 entries 512 entries 512 entries                    │
    (8 bytes    (each maps  (each maps                    │
     each)      2MB block    4KB page)                    │

Stage-2 Descriptor Format (Block/Page):
  ┌──────────────────────────────────────────────────────────────┐
  │ [63:52] Upper attributes                                     │
  │   [54]  XN — Execute-never for unprivileged                 │
  │   [53]  PXN — Execute-never for privileged (ARMv8.2-TTS2UXN)│
  │ [51:48] Reserved                                             │
  │ [47:12] Output PA (physical address of page/block)          │
  │ [11:10] Reserved                                             │
  │ [9:8]   SH — Shareability (00=Non, 10=Outer, 11=Inner)     │
  │ [7:6]   S2AP — Stage-2 Access Permissions:                  │
  │           00 = No access                                     │
  │           01 = Read-only                                     │
  │           10 = Write-only (rare)                             │
  │           11 = Read/Write                                    │
  │ [5:2]   MemAttr — Memory type:                              │
  │           0000 = Device-nGnRnE                               │
  │           0101 = Normal WB Cacheable                         │
  │           1111 = Normal WB RA/WA                            │
  │ [1:0]   Type: 01=Block, 11=Page/Table                       │
  └──────────────────────────────────────────────────────────────┘
```

---

## 4. Stage-2 Fault Handling

```
When a guest access causes a Stage-2 fault:

  1. Guest accesses IPA 0x9000_0000
  2. Stage-2 walk: no mapping found (or permission denied)
  3. Exception to EL2 (hypervisor)
  4. ESR_EL2 contains:
     • EC = 0x24 (Data Abort from lower EL)
     • ISS.ISV = data abort syndrome valid
     • ISS.SAS = access size (byte/half/word/double)
     • ISS.WnR = write(1) or read(0)
  5. HPFAR_EL2 = faulting IPA (shifted right by 12)
  6. FAR_EL2 = faulting VA

Common Stage-2 fault types:
┌─────────────────────┬────────────────────────────────────────┐
│ Fault               │ Hypervisor action                      │
├─────────────────────┼────────────────────────────────────────┤
│ Translation fault   │ Demand paging: allocate PA, map IPA→PA│
│ (no mapping)        │ (lazy memory allocation for VMs)       │
├─────────────────────┼────────────────────────────────────────┤
│ Permission fault    │ Copy-on-write: duplicate page, map R/W │
│ (write to R/O)     │ Or: dirty page tracking for migration  │
├─────────────────────┼────────────────────────────────────────┤
│ Access flag fault   │ Track accessed pages (working set)     │
│ (ARMv8.1-TTHM)     │ Used for page reclamation              │
├─────────────────────┼────────────────────────────────────────┤
│ MMIO access         │ No mapping at IPA of device registers  │
│ (device emulation) │ Decode access from ESR, emulate device │
└─────────────────────┴────────────────────────────────────────┘

MMIO Emulation example:
  Guest writes to IPA 0x0900_0000 (PL011 UART data register)
  → Stage-2 fault (no mapping)
  → Hypervisor:
    ESR_EL2.ISS.SRT = source register (e.g., X5)
    ESR_EL2.ISS.WnR = 1 (write)
    HPFAR_EL2 = 0x09000 (IPA >> 12)
    Value = guest's X5 register value
  → Hypervisor writes to real UART (or virtual console)
  → ERET to guest (advance PC by 4)
```

---

## 5. Device Passthrough & IOMMU (SMMUv3)

```
For high-performance I/O, devices can be passed directly to VMs
using an IOMMU (System MMU / SMMUv3):

  Without passthrough:                With passthrough:
  ┌──────┐                            ┌──────┐
  │ VM   │                            │ VM   │
  │  │   │                            │  │   │
  │  ├─ virtio ─► hypervisor          │  ├─ direct device access
  │  │          │                      │  │
  └──────┘      ▼                      └──────┘
            real device                     │
                                            ▼
                                       ┌──────────┐
                                       │ SMMUv3   │
                                       │(IPA→PA   │
                                       │ for DMA) │
                                       └────┬─────┘
                                            ▼
                                       real device

SMMUv3 provides:
  • Stage-1 translation (device VA → IPA)
  • Stage-2 translation (IPA → PA)
  • Stream ID → context lookup (which VM owns the device)
  • Fault reporting (device tries to access wrong memory)
  • PCIe ATS (Address Translation Service) support
  
  Result: Device DMA goes through SMMU, which enforces
  that VM can only access its own physical memory.
  VM gets near-native I/O performance.

SMMUv3 Components:
  ┌──────────────────────────────────────────────────────────┐
  │  Stream Table: StreamID → STE (Stream Table Entry)       │
  │    STE contains: Stage-1 table ptr + Stage-2 table ptr   │
  │                                                           │
  │  Command Queue: hypervisor → SMMU commands               │
  │    TLBI, CFGI (config), SYNC                             │
  │                                                           │
  │  Event Queue: SMMU → hypervisor (faults, errors)         │
  └──────────────────────────────────────────────────────────┘
```

---

## 6. Stage-2 for Memory Attributes

```
Stage-2 can override memory attributes from Stage-1:

  Combining rules:
  ┌────────────────┬──────────────┬──────────────────────────┐
  │ Stage-1 Type   │ Stage-2 Type │ Result                   │
  ├────────────────┼──────────────┼──────────────────────────┤
  │ Normal WB      │ Normal WB    │ Normal WB (best case)   │
  │ Normal WB      │ Normal WT    │ Normal WT (stricter)    │
  │ Normal WB      │ Normal NC    │ Normal NC (strictest)   │
  │ Normal (any)   │ Device       │ Device (overrides!)     │
  │ Device         │ Normal       │ Device (S1 wins)        │
  │ Device         │ Device       │ Most restrictive Device │
  └────────────────┴──────────────┴──────────────────────────┘

  ARMv8.4-S2FWB (Stage-2 Forced Write-Back):
    HCR_EL2.FWB=1 → Stage-2 can FORCE the memory type
    Without FWB: result = combine(S1, S2) — complex
    With FWB:    result = S2 attribute directly — simple
    
    Benefit: Hypervisor has full control over guest memory types
```

---

## 7. Dirty Page Tracking (for Live Migration)

```
Live migration moves a running VM from one host to another:

  1. Mark all guest pages as read-only in Stage-2
  2. Start copying pages to destination host
  3. Guest writes a page → Stage-2 permission fault
  4. Hypervisor:
     - Record page as dirty (write-modified)
     - Mark page R/W in Stage-2
     - ERET to guest (guest retries write — succeeds)
  5. After initial copy: re-copy only dirty pages
  6. Repeat until dirty page rate is low enough
  7. Pause VM, copy remaining dirty pages, resume on new host

  Dirty bitmap:
  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐
  │0│1│0│0│1│1│0│0│0│1│0│0│0│0│1│0│  (1 = dirty page)
  └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘
  
  KVM uses: KVM_GET_DIRTY_LOG ioctl to retrieve bitmap
  
  ARMv8.1-TTHM: Hardware dirty bit in S2 descriptors (DBM)
    Instead of fault on write, hardware sets dirty bit
    Hypervisor just scans descriptors — much faster!
```

---

Next: Back to [Virtualization Overview](./README.md) | Continue to [Debug & Trace Subsystem →](../07_Debug_Trace_Subsystem/)
