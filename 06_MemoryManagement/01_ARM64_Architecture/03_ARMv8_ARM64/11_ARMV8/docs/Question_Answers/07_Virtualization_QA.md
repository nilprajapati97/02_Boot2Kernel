# Virtualization Subsystem — Questions & Answers

---

## Q1. [L1] What is ARM Virtualization Extensions? How does EL2 enable hypervisors?

**Answer:**

```
ARMv8 provides hardware virtualization support at Exception
Level 2 (EL2), enabling efficient hypervisor implementations.

  ┌─────────────────────────────────────────────────────────────┐
  │              ARMv8 Virtualization Architecture               │
  │                                                              │
  │  EL0: ┌──────────┐ ┌──────────┐ ┌──────────┐              │
  │       │ VM1 Apps │ │ VM2 Apps │ │ VM3 Apps │              │
  │       └────┬─────┘ └────┬─────┘ └────┬─────┘              │
  │  EL1: ┌────┴─────┐ ┌────┴─────┐ ┌────┴─────┐              │
  │       │ Guest OS │ │ Guest OS │ │ Guest OS │              │
  │       │ (Linux)  │ │ (Windows)│ │ (Linux)  │              │
  │       └────┬─────┘ └────┬─────┘ └────┴─────┘              │
  │            │             │             │                    │
  │  EL2: ┌───┴─────────────┴─────────────┴───┐               │
  │       │         Hypervisor (KVM)           │               │
  │       │  • Trap & emulate sensitive ops    │               │
  │       │  • Stage-2 translation             │               │
  │       │  • Virtual interrupts              │               │
  │       │  • Timer virtualization            │               │
  │       └────────────────────────────────────┘               │
  │                                                              │
  │  EL3: ┌────────────────────────────────────┐               │
  │       │  Secure Monitor (TF-A / BL31)      │               │
  │       └────────────────────────────────────┘               │
  └─────────────────────────────────────────────────────────────┘

Key EL2 capabilities:
  1. Trapping: HCR_EL2 controls what guest operations trap to EL2
  2. Stage-2 translation: IPA → PA (Guest Physical → Real Physical)
  3. Virtual interrupts: inject IRQ/FIQ/SError into guest
  4. Timer virtualization: virtual timer offset
  5. VMID: Tag TLB entries per-VM (like ASID for address spaces)

HCR_EL2 critical trap bits:
  ┌──────┬──────────────────────────────────────────────────────┐
  │ Bit  │ Effect when set                                     │
  ├──────┼──────────────────────────────────────────────────────┤
  │ VM   │ Enable Stage-2 translation                          │
  │ SWIO │ Set/Way Invalidate Override (trap cache ops)        │
  │ FMO  │ Route FIQ to EL2                                    │
  │ IMO  │ Route IRQ to EL2                                    │
  │ AMO  │ Route SError to EL2                                 │
  │ TWI  │ Trap WFI to EL2                                     │
  │ TWE  │ Trap WFE to EL2                                     │
  │ TID3 │ Trap ID register reads (ID_AA64*)                   │
  │ TSC  │ Trap SMC to EL2                                     │
  │ TVM  │ Trap virtual memory register writes                 │
  │ RW   │ EL1 execution state (1=AArch64)                     │
  │ E2H  │ Enable VHE (Host runs at EL2)                       │
  │ TGE  │ Trap General Exceptions to EL2                      │
  └──────┴──────────────────────────────────────────────────────┘
```

---

## Q2. [L2] What is Stage-2 translation? How does the two-stage address translation work?

**Answer:**

```
Stage-2 translation adds a second level of address translation
controlled by the hypervisor, giving each VM its own "physical"
address space.

Two-stage translation:
  ┌──────────────────────────────────────────────────────────┐
  │                                                          │
  │  Guest (EL1):                                           │
  │    VA ──[Stage-1 MMU]──→ IPA (Intermediate Physical)    │
  │    Controlled by: TTBR0_EL1, TTBR1_EL1, TCR_EL1       │
  │    Page tables: owned by guest OS                       │
  │                                                          │
  │  Hypervisor (EL2):                                      │
  │    IPA ──[Stage-2 MMU]──→ PA (Physical Address)         │
  │    Controlled by: VTTBR_EL2, VTCR_EL2                  │
  │    Page tables: owned by hypervisor                     │
  │                                                          │
  │  Complete walk:                                         │
  │    VA → S1 walk → IPA → S2 walk → PA                   │
  │                                                          │
  │  But S1 page table entries are also in memory!          │
  │  So S1 walk itself needs S2 translation!                │
  │                                                          │
  │  Full walk for 4-level S1 + 3-level S2:                 │
  │    VA → S1-L0 PTE (IPA→S2 walk→PA) → read PTE          │
  │       → S1-L1 PTE (IPA→S2 walk→PA) → read PTE          │
  │       → S1-L2 PTE (IPA→S2 walk→PA) → read PTE          │
  │       → S1-L3 PTE (IPA→S2 walk→PA) → read PTE          │
  │       → Final IPA (IPA→S2 walk→PA) → data access       │
  │                                                          │
  │  Worst case: 4 × 4 + 4 = 24 memory accesses!           │
  │  (Each S1 level: 4 S2 walks; final data: 4 S2 walks)   │
  │  TLB caching is CRITICAL for performance.               │
  └──────────────────────────────────────────────────────────┘

VTTBR_EL2 (Virtualization Translation Table Base Register):
  [47:1]  = Stage-2 page table base address (PA)
  [63:48] = VMID (Virtual Machine Identifier)
             Up to 16-bit VMID (65536 VMs without TLB flush)

VTCR_EL2 (Virtualization Translation Control Register):
  T0SZ:  IPA size (e.g., 24 → 40-bit IPA space)
  SL0:   Starting level of Stage-2 walk
  IRGN0/ORGN0/SH0: cacheability and shareability
  PS:    Physical address size

Stage-2 permissions:
  S2AP[1:0]: 00=none, 01=read-only, 10=write-only, 11=RW
  XN:   Execute Never for guest
  
  Use cases:
    Memory: S2AP=RW, XN=0
    MMIO emulation: S2AP=none → fault → trap to hypervisor
    CoW for VM: S2AP=read-only → write fault → copy page
    Memory ballooning: unmap pages (S2AP=none)
```

---

## Q3. [L2] What is VHE (Virtualization Host Extensions)? How does KVM use it?

**Answer:**

```
VHE (ARMv8.1) allows the host kernel to run directly at EL2,
eliminating the cost of world-switching between EL1 and EL2.

Without VHE (traditional):
  ┌──────────────────────────────────────────────────────────┐
  │  EL2: KVM hypervisor (thin layer)                       │
  │  EL1: Host Linux kernel (world-switches between host/guest)│
  │  EL0: User applications                                 │
  │                                                          │
  │  Problem: KVM must switch EL1↔EL2 frequently            │
  │    Guest runs at EL1 (normal)                           │
  │    Host runs at EL1 (normal)                            │
  │    KVM trap handler at EL2 → switches context            │
  │    Every VM exit: EL1→EL2→EL1 = expensive!              │
  └──────────────────────────────────────────────────────────┘

With VHE (HCR_EL2.E2H=1, TGE=1):
  ┌──────────────────────────────────────────────────────────┐
  │  EL2: Host Linux kernel + KVM (runs here directly!)     │
  │  EL1: Guest OS only                                     │
  │  EL0: Host apps (TGE=1) or Guest apps (TGE=0)          │
  │                                                          │
  │  Magic: EL2 behaves LIKE EL1 for the host kernel        │
  │    TTBR0_EL1 → actually accesses TTBR0_EL2             │
  │    TTBR1_EL1 → actually accesses TTBR1_EL2             │
  │    SCTLR_EL1 → actually accesses SCTLR_EL2             │
  │    VBAR_EL1  → actually accesses VBAR_EL2              │
  │    ... and many more!                                    │
  │                                                          │
  │  Result: host kernel runs at EL2 without modification!  │
  │  No EL1↔EL2 switching for host operations              │
  │  Guest still runs at EL1 (need context switch)          │
  └──────────────────────────────────────────────────────────┘

Register redirection with VHE:
  When E2H=1 and TGE=1 (host context):
    MRS X0, TTBR0_EL1     → actually reads TTBR0_EL2
    MSR SCTLR_EL1, X0     → actually writes SCTLR_EL2
    MRS X0, TCR_EL1       → actually reads TCR_EL2
  
  When E2H=1 and TGE=0 (guest context):
    Guest's MRS/MSR to _EL1 → access REAL EL1 registers
    (no redirection — guest gets its own EL1 registers)

KVM with VHE — guest entry/exit:
  // Guest entry: (host at EL2, guest runs at EL1)
  1. Save host EL2 context (TTBR, SCTLR, etc.)
  2. Load guest EL1 context from VCPU struct
  3. Set HCR_EL2.TGE=0 (stop redirecting to EL2)
  4. Set VTTBR_EL2 with guest's Stage-2 table + VMID
  5. ERET to EL1 → guest starts executing
  
  // Guest exit: (trap to EL2)
  6. Save guest EL1 context to VCPU struct
  7. Load host EL2 context
  8. Set HCR_EL2.TGE=1 (redirect back to EL2)
  9. Handle exit reason
  10. Return to host or re-enter guest

Performance benefit:
  Without VHE: ~1000 cycles per VM exit (EL1→EL2 context switch)
  With VHE:    ~600 cycles (host already at EL2, less switching)
  → ~40% improvement in VM exit latency
```

---

## Q4. [L3] How does SMMU (System MMU / IOMMU) work for device virtualization?

**Answer:**

```
SMMU provides address translation and isolation for DMA-capable
devices, essential for safe device passthrough to VMs.

Why SMMU?
  Without SMMU: device does DMA to physical address
    → Compromised device can read/write ANY memory!
    → Guest VM gets real PA → breaks isolation!
  
  With SMMU: device DMA goes through translation
    → Device uses Virtual Address or IPA
    → SMMU translates to real PA
    → Access control enforced per-device

SMMU Architecture (SMMUv3):
  ┌──────────────────────────────────────────────────────────┐
  │                    SMMUv3 Pipeline                       │
  │                                                          │
  │  Device → StreamID → Stream Table → Context Descriptor  │
  │                                     │                    │
  │                        ┌────────────┘                    │
  │                        ▼                                 │
  │              ┌──────────────────────┐                    │
  │              │ Stage-1 Translation  │ (device virtual →  │
  │              │  (per-device tables) │  intermediate PA)  │
  │              └──────────┬───────────┘                    │
  │                         ▼                                │
  │              ┌──────────────────────┐                    │
  │              │ Stage-2 Translation  │ (IPA → PA,         │
  │              │  (shared with CPU S2)│  same as CPU VM)   │
  │              └──────────┬───────────┘                    │
  │                         ▼                                │
  │                    Physical Address → Memory             │
  └──────────────────────────────────────────────────────────┘

Stream Table:
  Indexed by StreamID (from PCIe BDF or device-specific ID)
  Each entry (STE) contains:
    • Valid bit
    • Config: bypass, Stage-1, Stage-2, or both
    • S1ContextPtr: pointer to Context Descriptor
    • S2TTB: Stage-2 table base (if Stage-2 enabled)
    • VMID: Virtual Machine ID

  Linear Stream Table: array indexed by StreamID
  2-level Stream Table: for large StreamID spaces (PCIe)

Context Descriptor (CD):
  Per-device Stage-1 configuration:
    • TTB0: page table base for this device
    • TCR: translation control
    • ASID: address space ID for this device
    • Allows per-device address spaces!

VFIO device passthrough:
  ┌──────────────────────────────────────────────────────┐
  │ 1. PCIe NVMe assigned to VM via VFIO                │
  │ 2. SMMU STE: StreamID(NVMe BDF) → Stage-2 only     │
  │ 3. Stage-2 table: SAME as guest's CPU Stage-2       │
  │    → Device sees same IPA space as guest CPU         │
  │ 4. Guest configures NVMe DMA to Guest Physical Addr │
  │ 5. NVMe DMA → SMMU S2 → real PA → correct memory!  │
  │ 6. Guest can't program NVMe to access other VM's RAM│
  │    → SMMU S2 blocks access outside VM's IPA range   │
  └──────────────────────────────────────────────────────┘

SMMUv3 features:
  • 2-stage translation (Stage-1 + Stage-2)
  • PCIe ATS (Address Translation Service) support
  • PRI (Page Request Interface) for on-demand paging
  • HTTU (Hardware Translation Table Update) for A/D bits
  • MSI remapping
  • STALL model for recoverable faults
```

---

## Q5. [L2] What is VMID and how is it used in virtualization?

**Answer:**

```
VMID (Virtual Machine Identifier) tags TLB entries per-VM,
allowing TLB entries from different VMs to coexist.

Without VMID:
  Hypervisor must flush TLB on every VM switch
  → Expensive! Cold TLB for every guest entry
  
With VMID:
  TLB entry: {VMID, VA, IPA, PA, permissions}
  Different VMs use different VMIDs
  TLB lookup: match VMID + VA → correct translation per VM
  → No TLB flush on VM switch! Just change VMID.

VMID in VTTBR_EL2:
  ┌──────────────────────────────────────────────────────────┐
  │ VTTBR_EL2:                                              │
  │  [63:48] = VMID (8 or 16 bits)                         │
  │  [47:1]  = BADDR (Stage-2 table base address)          │
  │                                                          │
  │  VTCR_EL2.VS: 0 = 8-bit VMID (256 VMs)                │
  │                1 = 16-bit VMID (65536 VMs)              │
  │                                                          │
  │  VM switch:                                             │
  │    Just write new VTTBR_EL2 with:                       │
  │      New VMID + new Stage-2 table pointer               │
  │    No TLB invalidation needed (VMIDs differ)            │
  └──────────────────────────────────────────────────────────┘

Combined TLB tagging:
  Each TLB entry is tagged with:
    VMID (which VM)  + ASID (which process within VM)
  
  Lookup requires matching BOTH:
    VMID=5, ASID=42, VA=0x400000 → PA=0x8000000
    VMID=5, ASID=43, VA=0x400000 → PA=0x9000000  (different process)
    VMID=6, ASID=42, VA=0x400000 → PA=0xA000000  (different VM)

VMID allocation strategies:
  1. Static: VM gets fixed VMID at creation
     Simple, but limited to 256/65536 VMs
  
  2. Dynamic: allocate from pool, recycle with TLB flush
     More VMs than VMIDs → evict using TLBI VMALLE1IS

TLB invalidation by VMID:
  TLBI VMALLS12E1IS   // Invalidate all Stage-1 and Stage-2
                       // entries for current VMID (inner-shareable)
  TLBI ALLE1IS        // All EL1 entries (current VMID)
  TLBI IPAS2E1IS, X0  // Invalidate IPA→PA for current VMID
```

---

## Q6. [L2] How does KVM/ARM handle VM exits (traps to EL2)? Common exit reasons?

**Answer:**

```
When a guest executes a sensitive instruction or accesses a
trapped resource, hardware traps to EL2 (VM exit).

Exit flow:
  ┌──────────────────────────────────────────────────────────┐
  │ 1. Guest executes trapped instruction at EL1            │
  │ 2. Hardware: save guest state → jump to VBAR_EL2 vector │
  │ 3. KVM: save remaining guest state to vcpu struct       │
  │ 4. KVM: read ESR_EL2 for exit reason                   │
  │ 5. KVM: handle exit (emulate, forward, etc.)           │
  │ 6. KVM: restore guest state → ERET back to guest       │
  └──────────────────────────────────────────────────────────┘

ESR_EL2 (Exception Syndrome Register at EL2):
  EC field [31:26] identifies the trap reason:
  
  ┌───────┬──────────────────────────────────────────────────┐
  │ EC    │ Reason                                          │
  ├───────┼──────────────────────────────────────────────────┤
  │ 0x01  │ WFI/WFE trap (HCR_EL2.TWI/TWE)                │
  │ 0x07  │ SVE/SIMD/FP access trap                        │
  │ 0x16  │ HVC from EL1 (hypercall from guest)            │
  │ 0x17  │ SMC from EL1 (trapped by HCR_EL2.TSC)         │
  │ 0x18  │ System register access trap (MSR/MRS)          │
  │ 0x20  │ Instruction Abort from lower EL (Stage-2 fault)│
  │ 0x24  │ Data Abort from lower EL (Stage-2 fault)       │
  │ 0x2C  │ Floating-point exception                       │
  └───────┴──────────────────────────────────────────────────┘

Common exit handling:

  1. Stage-2 fault (EC=0x24):
     • Guest accesses MMIO (unmapped in Stage-2)
     • KVM: decode instruction, emulate device access
     • Example: guest writes to emulated UART register
     
  2. WFI trap (EC=0x01):
     • Guest idle → blocked on WFI
     • KVM: block VCPU, schedule another VCPU or idle host
     • Important for power: don't spin waiting for guest wake
     
  3. System register trap (EC=0x18):
     • Guest reads/writes trapped system register
     • KVM: emulate (return fake value or capture write)
     • Example: guest reads MIDR_EL1 → return emulated CPU ID
     
  4. HVC (EC=0x16):
     • Guest makes intentional hypercall
     • KVM: handle para-virtualized operation
     • Example: virtio notification, PSCI call
     
  5. SMC trap (EC=0x17):
     • Guest tried SMC (SecureMonitor Call)
     • KVM: forward PSCI calls to TF-A, or reject

Exit reduction strategies:
  • Map all guest RAM in Stage-2 → no data abort exits
  • Use VHE → host at EL2, fewer context switches
  • GICv4 direct injection → no interrupt exits
  • Para-virtualization (virtio) → fewer MMIO traps
```

---

## Q7. [L3] How does nested virtualization work on ARM (FEAT_NV2)?

**Answer:**

```
Nested virtualization allows running a hypervisor inside a VM.
FEAT_NV/NV2 (ARMv8.3/8.4) provides hardware support.

Use case:
  L0 (bare metal): KVM host
    L1 (VM): another KVM instance (guest hypervisor)
      L2 (nested VM): guest OS inside L1's VM

Without hardware support:
  L1 hypervisor (at EL1) tries to:
    Access EL2 system registers → trap to L0 (at EL2)
    Execute HVC → trap to L0
    Configure Stage-2 → trap to L0
  
  EVERY EL2 operation → expensive trap!
  L0 must emulate ALL of EL2 for L1.

With FEAT_NV2:
  ┌──────────────────────────────────────────────────────────┐
  │ HCR_EL2.NV=1, NV2=1:                                   │
  │                                                          │
  │ L1 hypervisor runs at EL1 but:                          │
  │   • Can access EL2 system registers                    │
  │   • Reads/writes go to a memory-backed structure        │
  │     called VNCR (Virtual Nested Control Register) page  │
  │   • VNCR_EL2: points to memory page with EL2 reg values│
  │                                                          │
  │ When L1 does MSR VTTBR_EL2, X0:                        │
  │   → Hardware writes X0 to VNCR page at VTTBR offset    │
  │   → No trap! (handled in hardware/memory)              │
  │                                                          │
  │ When L1 does ERET to enter L2:                          │
  │   → Hardware reads VNCR page for EL2 configuration     │
  │   → Sets up nested Stage-2 using values from VNCR      │
  │   → Runs L2 with proper virtualization                  │
  │                                                          │
  │ L2 VM exit:                                              │
  │   → Trap to L0 (real EL2)                               │
  │   → L0: update VNCR page for L1                        │
  │   → L0: inject virtual exception to L1                 │
  │   → L1 handles the "virtual VM exit"                   │
  └──────────────────────────────────────────────────────────┘

Translation for nested VMs:
  L2 VA → L1-S1 (L2's Stage-1) → L2 IPA
  L2 IPA → L1-S2 (L1's Stage-2) → L1 IPA  
  L1 IPA → L0-S2 (L0's Stage-2) → PA
  
  3 levels of translation! Hardware walks are expensive.
  Shadow page tables or merged Stage-2 used to optimize.

Performance with FEAT_NV2:
  Without NV2: ~10x overhead for nested VM operations
  With NV2: ~2-3x overhead (VNCR eliminates most traps)
```

---

## Q8. [L2] How does timer virtualization work in ARM?

**Answer:**

```
ARM provides virtual timers that allow each VM to have its own
independent timer without hypervisor intervention.

ARM timers:
  ┌──────────────────────────────────────────────────────────┐
  │ Physical Timers:                                        │
  │   CNTPCT_EL0:         Physical count (system counter)  │
  │   CNTP_TVAL_EL0:      Physical timer value             │
  │   CNTP_CTL_EL0:       Physical timer control           │
  │   CNTP_CVAL_EL0:      Physical timer compare value     │
  │                                                          │
  │ Virtual Timers:                                         │
  │   CNTVCT_EL0:         Virtual count (= CNTPCT - offset)│
  │   CNTV_TVAL_EL0:      Virtual timer value              │
  │   CNTV_CTL_EL0:       Virtual timer control            │
  │   CNTV_CVAL_EL0:      Virtual timer compare value      │
  │                                                          │
  │ Hypervisor Timer:                                       │
  │   CNTHP_*_EL2:        Hypervisor physical timer        │
  │   CNTHV_*_EL2:        Hypervisor virtual timer (VHE)   │
  └──────────────────────────────────────────────────────────┘

Virtual timer offset:
  CNTVOFF_EL2: virtual counter offset (set by hypervisor)
  
  CNTVCT_EL0 = CNTPCT_EL0 - CNTVOFF_EL2
  
  Purpose: each VM sees time starting from 0 at VM creation
  
  VM1 created at physical count 1000000:
    CNTVOFF_EL2 = 1000000
    Guest reads CNTVCT_EL0 → gets 0 (freshly booted)
  
  VM2 created at physical count 5000000:
    CNTVOFF_EL2 = 5000000
    Guest reads CNTVCT_EL0 → gets 0 (freshly booted)

Timer trap control:
  CNTHCTL_EL2:
    EL1PCTEN: 1=allow EL1/EL0 access to physical counter
    EL1PCEN:  1=allow EL1/EL0 access to physical timer
    
  Typical KVM setup:
    EL1PCTEN=1: guest can read physical counter (rdtsc equivalent)
    EL1PCEN=0: guest can't access physical timer (use virtual)
    
  Guest uses virtual timer → fires PPI INTID 27
  → No trap needed! Hardware handles timer expiry.
  → Virtual timer interrupt delivered directly to guest.

Timer migration:
  When VCPU migrates to different physical CPU:
    1. Cancel physical timer on old CPU
    2. Read remaining ticks from CNTV_CVAL_EL0
    3. Restore CNTV_CVAL_EL0 on new CPU
    4. Set CNTVOFF_EL2 (same offset, different physical counter)
    → Guest timer continues seamlessly
```

---

## Q9. [L2] What is the difference between full virtualization and para-virtualization on ARM? How does virtio fit?

**Answer:**

```
Full virtualization:
  Guest OS runs UNMODIFIED
  All device access → Stage-2 fault → hypervisor emulates
  Guest thinks it's on real hardware
  
  Example: guest accesses emulated PL011 UART at 0x09000000
    1. Guest: STR X0, [X1] (write to UART data register)
    2. Stage-2: address not mapped → Data Abort to EL2
    3. KVM: decode instruction, extract data value
    4. KVM: emulate UART register write (buffer char)
    5. KVM: advance guest PC
    6. ERET back to guest
    
  Cost: ~2000 cycles per emulated MMIO access!

Para-virtualization (virtio):
  Guest knows it's in a VM, uses optimized interface
  Shared memory + doorbell → minimal exits
  
  Example: virtio-blk (block device)
    1. Guest: prepare I/O request in shared virtqueue (memory)
    2. Guest: write to virtio MMIO doorbell (1 exit)
    3. KVM/Host: process all queued requests
    4. KVM/Host: update completion ring in shared memory
    5. KVM/Host: inject virtual IRQ to guest
    6. Guest: process completions from shared memory
    
  Cost: 1 exit per BATCH of I/O requests!
  → 100x fewer exits than emulated device

Virtio on ARM:
  ┌──────────────────────────────────────────────────────────┐
  │                 Virtio Architecture                      │
  │                                                          │
  │  Guest:                                                  │
  │    virtio-net driver                                    │
  │      │                                                   │
  │      ▼                                                   │
  │    virtqueue (shared memory ring buffer)                 │
  │      • avail ring: guest → host requests                │
  │      • used ring: host → guest completions              │
  │      • descriptor table: scatter-gather lists           │
  │      │                                                   │
  │      ▼                                                   │
  │    MMIO doorbell write (1 VM exit to notify host)       │
  │                                                          │
  │  Host:                                                   │
  │    vhost / vhost-user / vhost-net                       │
  │    Processes shared memory directly (zero-copy)         │
  │    Injects virtual IRQ for completions                  │
  └──────────────────────────────────────────────────────────┘

  Transport options:
    virtio-mmio: MMIO-based (simple, device tree)
    virtio-pci:  PCI-based (standard, supports MSI-X)

  Common virtio devices:
    virtio-net:  network (10+ Gbps with vhost-net)
    virtio-blk:  block storage
    virtio-scsi: SCSI storage
    virtio-gpu:  graphics
    virtio-fs:   filesystem sharing (virtiofsd)
```

---

## Q10. [L2] How does ARM handle virtual interrupts for guests? What are List Registers?

**Answer:**

```
The hypervisor injects virtual interrupts to guests using
GIC List Registers (LRs) at EL2.

ICH_LR<n>_EL2: up to 16 List Registers
  Each LR represents one pending virtual interrupt for the guest.

  ┌──────────────────────────────────────────────────────────┐
  │ ICH_LR<n>_EL2 format:                                  │
  │                                                          │
  │ [63:62] State:  00=Invalid, 01=Pending,                 │
  │                 10=Active, 11=Pending+Active             │
  │ [61]    HW:     1=backed by physical interrupt          │
  │                 0=pure software virtual interrupt        │
  │ [60]    Group:  0=Group 0, 1=Group 1                    │
  │ [55:48] Priority: virtual priority value                │
  │ [44:32] pINTID: physical INTID (only when HW=1)        │
  │ [31:0]  vINTID: virtual INTID (what guest sees)        │
  └──────────────────────────────────────────────────────────┘

HW=1 (hardware-backed virtual interrupt):
  Physical interrupt is mapped 1:1 to virtual interrupt
  When guest EOIs the virtual interrupt:
    → Hardware automatically deactivates the PHYSICAL interrupt
    → No hypervisor involvement for EOI!
  
  Used for: assigned device interrupts (passthrough)
    Physical UART SPI→ HW=1 LR → guest handles directly

HW=0 (pure virtual interrupt):
  No physical interrupt backing
  When guest EOIs: maintenance interrupt to hypervisor
  (or hypervisor checks LR state on next VM exit)
  
  Used for: emulated device interrupts
    Emulated virtio completion → inject virtual IRQ

Injection flow:
  ┌──────────────────────────────────────────────────────────┐
  │ 1. Physical IRQ arrives → trap to EL2                   │
  │ 2. KVM: read ICC_IAR1_EL1 → physical INTID=33          │
  │ 3. KVM: map to VCPU → find virtual INTID for this VM   │
  │ 4. KVM: write ICH_LR0_EL2:                             │
  │    State=Pending, HW=1, pINTID=33, vINTID=33, Priority │
  │ 5. KVM: ERET to guest                                  │
  │ 6. Guest: CPU sees pending virtual IRQ                  │
  │ 7. Guest: reads ICC_IAR1_EL1 → gets vINTID=33          │
  │    (GIC returns vINTID from LR, not real IAR)           │
  │ 8. Guest: handles interrupt, writes ICC_EOIR1_EL1=33   │
  │ 9. Hardware: LR.HW=1 → auto-deactivate physical INTID  │
  │ 10. LR state → Invalid (slot available for next IRQ)    │
  └──────────────────────────────────────────────────────────┘

ICH_HCR_EL2: Hypervisor Control Register
  EN: Enable virtual CPU interface
  UIE: Underflow Interrupt Enable (LRs almost empty)
  LRENPIE: LR Entry Not Present (no valid LR for pending vIRQ)
  NPIE: No Pending Interrupt (all LRs invalid)
```

---

Back to [Question & Answers Index](./README.md)
