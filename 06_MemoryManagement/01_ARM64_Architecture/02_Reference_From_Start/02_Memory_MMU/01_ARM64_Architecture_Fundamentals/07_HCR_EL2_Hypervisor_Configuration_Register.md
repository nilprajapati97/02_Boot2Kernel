# HCR_EL2 — Hypervisor Configuration Register

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm (KVM/hypervisor roles)

---

## 1. Concept Foundation

`HCR_EL2` (Hypervisor Configuration Register, EL2) is the primary control register for the hypervisor. It controls:
- **Stage 2 translation**: Whether guest OS physical memory accesses are further translated via the hypervisor's page tables.
- **Exception routing**: Which exceptions generated at EL0/EL1 are intercepted and routed to EL2 instead of being handled by the guest OS.
- **Trap controls**: Which guest instructions/register accesses trigger a trap to EL2, enabling hypervisor emulation.
- **VHE mode**: Whether the host Linux kernel runs directly at EL2 (Virtualization Host Extensions).

Only EL2 and EL3 can read/write `HCR_EL2`. The guest OS (EL1) cannot see or modify it.

---

## 2. HCR_EL2 Key Bit Fields

```
Bit[63:34]  — Reserved (extension bits for ARMv8.2+ features)

Critical bits:
Bit[33]  ID     — Disables Stage 2 instruction cache maintenance traps
Bit[32]  CD     — Disables Stage 2 data cache maintenance traps
Bit[31]  RW     — Execution state for EL1/EL0 (0=AArch32, 1=AArch64)
Bit[30]  TRVM   — Trap reads of VM registers
Bit[29]  HCD    — HVC disable (1=HVC from EL1 = UNDEF)
Bit[28]  TDZ    — Trap DC ZVA instructions
Bit[27]  TGE    — Trap General Exceptions (1=all EL0 exceptions → EL2)
Bit[26]  TVM    — Trap virtual memory control registers writes
Bit[25]  TTLB   — Trap TLB maintenance operations
Bit[24]  TPU    — Trap cache/TLB Profiling Unit access
Bit[23]  TPCP   — Trap cache/prefetch control registers
Bit[22]  TSW    — Trap data cache invalidate by set/way
Bit[21]  TACR   — Trap Auxiliary Control Register
Bit[20]  TIDCP  — Trap implementation-defined CP register access
Bit[19]  TSC    — Trap SMC instructions (0=SMC works, 1=SMC from EL1 → EL2)
Bit[18]  TID3   — Trap identification register group 3
Bit[17]  TID2   — Trap identification register group 2
Bit[16]  TID1   — Trap identification register group 1
Bit[15]  TID0   — Trap identification register group 0
Bit[14]  TWE    — Trap WFE (Wait For Event) from EL0/EL1 to EL2
Bit[13]  TWI    — Trap WFI (Wait For Interrupt) from EL0/EL1 to EL2
Bit[12]  DC     — Default cacheability for Stage 2 (1=Normal WB)
Bit[11:10] BSU  — Barrier Shareability Upgrade
Bit[9]   FB     — Force broadcast of maintenance operations
Bit[8]   VSE    — Virtual SError injection
Bit[7]   VI     — Virtual IRQ pending
Bit[6]   VF     — Virtual FIQ pending
Bit[5]   AMO    — Physical SError routing: 0=EL1, 1=EL2
Bit[4]   IMO    — Physical IRQ routing: 0=EL1, 1=EL2
Bit[3]   FMO    — Physical FIQ routing: 0=EL1, 1=EL2
Bit[2]   PTW    — Protected Table Walk
Bit[1]   SWIO   — Set/Way Invalidation Override (treat as PoU+PoC)
Bit[0]   VM     — Stage 2 MMU enable (0=disabled, 1=Stage 2 translation active)
```

---

Here’s a **clean graphical / block-style view** of `HCR_EL2` so you can *see how the bits are organized and grouped*, instead of just reading a long list.

---

# 🧩 HCR_EL2 — 64-bit Register Layout (Visual)

```
 63                                                             32 31                              0
 ┌───────────────────────────────────────────────────────────────┬───────────────────────────────────┐
 │        Feature / Extension / Advanced Controls (v8.1+)        │    Core Virtualization Controls   │
 │                                                               │                                   │
 │  MTE | NV | ATA | DCT | TME | TERR | TEA | TLOR | E2H | CD/ID │ RW  | TRVM| HCD | TDZ| TGE | TVM  │
 │                                                               │ TTLB| TPU | TPC | TSW| TACR|...   │
 │                                                               │ TIDx| TWE | TWI | DC | BSU | FB   │
 │                                                               │ VSE | VI  | VF  | AMO| IMO | FMO  │
 │                                                               │ PTW | SWIO| VM                    │
 └───────────────────────────────────────────────────────────────┴───────────────────────────────────┘
```

---

# 🔍 Zoomed View — Lower 32 Bits (Most Important)

This is where **90% of hypervisor work happens**:

```
Bit: 31                                                        0
     ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────────────┐
     │ RW │TRVM│HCD │TDZ │TGE │TVM │TTLB│TPU │TPC │ ...        │
     ├────┼────┼────┼────┼────┼────┼────┼────┼────┼────────────┤
     │TSW │TACR│TIDCP│TSC │TID3│TID2│TID1│TID0│TWE │TWI        │
     ├────┼────┼────┼────┼────┼────┼────┼────┼────┼────────────┤
     │ DC │ BSU[1:0] │ FB │ VSE │ VI │ VF │ AMO │ IMO │ FMO    │
     ├────┼──────────┼────┼────┼────┼────┼────┼────┼───────────┤
     │ PTW│ SWIO │ VM                                          │
     └────┴──────┴─────────────────────────────────────────────┘
```

---

# 🧠 Functional Block View (Much Easier to Understand)

Instead of bits, think in **functional blocks**:

```
                ┌──────────────────────────────┐
                │   Execution Control          │
                │   RW, E2H, TGE               │ 
                └────────────┬─────────────────┘
                             │
                             ▼
 ┌──────────────────────────────────────────────────────────────┐
 │              Virtualization Core                             │
 │              VM, PTW, DC                                     │
 └──────────────────────────────────────────────────────────────┘
                             │
                             ▼
 ┌──────────────────────────────────────────────────────────────┐
 │              Trap & Intercept Engine                         │
 │  TVM, TTLB, TWI, TWE, TSC, TIDx, HCD, TRVM                   │
 └──────────────────────────────────────────────────────────────┘
                             │
                             ▼
 ┌──────────────────────────────────────────────────────────────┐
 │              Interrupt Virtualization                        │
 │        FMO, IMO, AMO, VI, VF, VSE                            │
 └──────────────────────────────────────────────────────────────┘
                             │
                             ▼
 ┌──────────────────────────────────────────────────────────────┐
 │              Memory & Cache Control                          │
 │     BSU, FB, TPC, TPU, TSW, CD, ID                           │
 └──────────────────────────────────────────────────────────────┘
                             │
                             ▼
 ┌──────────────────────────────────────────────────────────────┐
 │              Advanced / Extensions                           │
 │   MTE, NV, PAC, TME, TERR, TEA, ATA, DCT                     │
 └──────────────────────────────────────────────────────────────┘
```

---

# ⚙️ Data Flow View (How It Actually Works)

This shows how `HCR_EL2` affects execution:

```
        Guest OS (EL1)
              │
              │ executes instruction
              ▼
     ┌───────────────────────┐
     │   Check HCR_EL2       │
     │  (Trap? Route? Allow?)│
     └─────────┬─────────────┘
               │
     ┌─────────┼─────────────┐
     │         │             │
     ▼         ▼             ▼
 Allowed   Trap to EL2   Inject Virtual Interrupt
 (normal)   (hypervisor)     (VI/VF/VSE)
```

---

# 🎯 Minimal “Real Hypervisor” View

If you strip everything down, this is what matters most:

```
        HCR_EL2 (practical core)
        ┌───────────────────────┐
        │ VM   → enable VM      │
        │ RW   → 64-bit guest   │
        │ TVM  → trap MMU ops   │
        │ IMO/FMO → interrupts  │
        │ TWI/TWE → trap sleep  │
        │ TTLB → trap TLB ops   │
        │ E2H → host mode       │
        └───────────────────────┘
```

---

# 🧠 Intuition (Best Way to Remember)

Think of `HCR_EL2` like a **hypervisor control matrix**:

* **Top bits** → advanced CPU features
* **Middle bits** → trap & control guest behavior
* **Bottom bits** → core virtualization + interrupts

---

## 3. Most Important Bits Explained

### Bit 0 — VM (Stage 2 MMU Enable)

**VM=0**: Stage 2 translation disabled. Guest OS's EL1 physical addresses are the actual physical addresses. This is the state before KVM configures the guest.

**VM=1**: Stage 2 translation enabled. Guest EL1 treats its physical addresses as IPAs (Intermediate Physical Addresses). The hypervisor's Stage 2 page tables (`VTTBR_EL2`) further translate IPA → PA. This is how KVM isolates guests from each other and from the host.

```c
// arch/arm64/kvm/hyp/vhe/switch.c
static void activate_traps_vhe(struct kvm_vcpu *vcpu)
{
    u64 hcr = vcpu->arch.hcr_el2;
    // Set VM=1 before ERET to guest
    hcr |= HCR_VM;
    write_sysreg(hcr, hcr_el2);
    isb();
}
```

### Bit 31 — RW (Register Width / Execution State)

**RW=0**: EL1 and EL0 run in AArch32 mode. Used for 32-bit guest VMs.
**RW=1**: EL1 and EL0 run in AArch64 mode. Standard for Linux VMs on ARM64.

### Bit 4 — IMO (IRQ Override / Physical IRQ to EL2)

**IMO=0**: Physical IRQs from EL0/EL1 context go to EL1 (guest OS handles them).
**IMO=1**: Physical IRQs are routed to EL2. Used in conjunction with virtual IRQ injection (VI bit) to implement virtual interrupt delivery to the guest.

Under KVM:
- Host IRQs during guest execution: `IMO=1` routes them to EL2 first → host Linux GIC driver handles → KVM determines if it's a guest IRQ → injects via `HCR_EL2.VI=1`.

### Bit 3 — FMO (FIQ Override)
### Bit 5 — AMO (SError Override)

Similar to IMO but for FIQ and SError. KVM typically sets all three (IMO, FMO, AMO = 1) so all physical interrupts are routed to EL2 during guest execution.

### Bit 7 — VI (Virtual IRQ Pending)

**VI=1**: A virtual IRQ is pending for the guest. When the guest's PSTATE has I-bit clear (interrupts unmasked), the virtual IRQ fires as if a real IRQ arrived. The guest sees it as a regular IRQ to its vector table (`VBAR_EL1 + 0x480` from lower EL).

This is the mechanism for delivering host-scheduled IRQs (e.g., virtual timer, virtio) to the guest without a full VM exit.

### Bit 27 — TGE (Trap General Exceptions)

**TGE=1**: All EL0 exceptions are routed to EL2 instead of EL1. This is used in KVM's Type-1 model or when the hypervisor wants to intercept everything from EL0.

In standard KVM operation, `TGE=0` so EL0→EL1 exceptions are handled by the guest OS.

### Bit 26 — TVM (Trap Virtual Memory Registers)

**TVM=1**: Any guest OS write to EL1 virtual memory control registers (`SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `MAIR_EL1`, etc.) causes a trap to EL2. KVM uses this to shadow the guest's page tables and maintain consistency between Stage 1 and Stage 2 mappings.

### Bit 33,32 — ID, CD (Cache Disable for Stage 2)

These control whether guest Stage 2 cache maintenance operations propagate correctly.

---

## 4. VHE — Virtualization Host Extensions (ARMv8.1)

`HCR_EL2.E2H` (bit 34, ARMv8.1+) enables **VHE mode**:

```
E2H=0 (non-VHE): Host Linux kernel runs at EL1, KVM runs at EL2
                  Context switch overhead: EL1 ↔ EL2 for each vcpu entry/exit

E2H=1 (VHE):     Host Linux kernel runs at EL2 directly
                  No EL1/EL2 transition needed for host kernel
                  Access to EL1 registers "aliased" to EL2 registers
```

With VHE, when `HCR_EL2.E2H=1` and `HCR_EL2.TGE=1`, the CPU behaves as if EL2 is the "host kernel EL". Many EL1 register accesses (`SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`) are automatically redirected to their EL2 equivalents:

```
With VHE active (E2H=1, TGE=1):
  MSR SCTLR_EL1, x0   → actually writes SCTLR_EL2
  MRS x0, TCR_EL1     → actually reads TCR_EL2
  (Transparent redirection — no source changes needed in host kernel)
```

This means the host Linux kernel code runs unchanged at EL2.

When entering a guest, KVM sets `TGE=0` to make EL1 the guest kernel EL, and sets up Stage 2 with `VM=1`.

---

## 5. HCR_EL2 in KVM ARM64

```c
// arch/arm64/include/asm/kvm_host.h
// Default HCR_EL2 value for a guest vcpu:
#define HCR_GUEST_FLAGS (HCR_TSC | HCR_TSW | HCR_TWE | HCR_TWI | \
                         HCR_VM | HCR_BSU_IS | HCR_FB | HCR_TAC | \
                         HCR_AMO | HCR_IMO | HCR_FMO | \
                         HCR_SWIO | HCR_TIDCP | HCR_RW)

// VHE host flags:
#define HCR_HOST_VHE_FLAGS (HCR_E2H | HCR_TGE | HCR_FMO | HCR_IMO | \
                             HCR_AMO | HCR_RW | HCR_TACR | HCR_TIDCP)
```

### KVM Exit/Entry HCR_EL2 Changes

```c
// On entering a guest (vcpu_run):
static void kvm_arm_vcpu_load_sysregs_vhe(struct kvm_vcpu *vcpu)
{
    // Switch from host HCR to guest HCR
    write_sysreg(vcpu->arch.hcr_el2, hcr_el2);
    // This enables: VM=1, IMO=1, FMO=1, AMO=1, TSC=1
    // And disables: E2H=1, TGE=1 (only for VHE exit)
    isb();
}

// On exiting a guest (vm exit):
static void kvm_arm_vcpu_put_sysregs_vhe(struct kvm_vcpu *vcpu)
{
    // Restore host HCR
    write_sysreg(HCR_HOST_VHE_FLAGS, hcr_el2);
    isb();
}
```

---

## 6. BSU — Barrier Shareability Upgrade

`HCR_EL2.BSU[11:10]` upgrades the shareability domain of barriers issued by the guest:

```
BSU = 0b00: No upgrade (guest barriers have their specified shareability)
BSU = 0b01: Inner Shareable minimum (ISH at minimum)
BSU = 0b10: Outer Shareable minimum (OSH at minimum)
BSU = 0b11: Full system (SY)
```

KVM sets `BSU=IS` (Inner Shareable) to ensure guest DMB/DSB instructions are visible to the host.

---

## 7. WFI/WFE Trapping

`HCR_EL2.TWI` and `HCR_EL2.TWE` control whether `WFI` and `WFE` instructions from EL0/EL1 trap to EL2:

**TWI=1**: `WFI` from EL1/EL0 traps to EL2 as an undefined instruction equivalent. KVM uses this to intercept guest idle: instead of actually waiting for interrupt, KVM can schedule another vCPU or the host.

**TWE=1**: `WFE` similarly traps.

```c
// In KVM: when guest executes WFI
// → trap to EL2, ESR_EL2.EC = 0x01 (WFI/WFE trap)
// KVM handles this as a vcpu idle
// kvm_handle_wfx() in arch/arm64/kvm/handle_exit.c
```

---

## 8. Interview Questions & Answers

**Q1: What does HCR_EL2.VM=1 do and when is it set?**

`VM=1` enables Stage 2 address translation. When set, guest EL1 "physical" addresses (generated by guest Stage 1 page table walks) are treated as Intermediate Physical Addresses (IPAs). The CPU performs a second table walk using the Stage 2 page tables pointed to by `VTTBR_EL2`. This translates IPA → PA. KVM sets `VM=1` before executing the guest (entering the vcpu) and clears it (via `VM=0` in host HCR) after the guest exits. This provides memory isolation between different VMs.

**Q2: What is the difference between VHE and non-VHE KVM?**

In non-VHE, the host Linux kernel runs at EL1 and KVM runs at EL2. Every guest entry/exit requires an EL1↔EL2 transition (expensive context switch). In VHE (`HCR_EL2.E2H=1`), the host Linux kernel runs at EL2 directly. EL1 register accesses from host code are transparently redirected to EL2 equivalents. Guest entry/exit only requires changing `HCR_EL2.TGE` and `VM`, not a full EL change for the host kernel. This significantly reduces vcpu entry/exit latency.

**Q3: How does KVM deliver a virtual IRQ to a guest?**

KVM sets `HCR_EL2.VI=1` (Virtual IRQ pending). When the guest's PSTATE.I=0 (IRQs unmasked), the CPU delivers a virtual IRQ to the guest's vector table at `VBAR_EL1 + 0x480`. The guest's interrupt handler runs normally, queries the virtual GIC (VGIC) to get the interrupt ID, and processes it. No EL2 entry is required for the interrupt delivery itself — only for the initial VI injection.

**Q4: Why does KVM set HCR_EL2.TSC=1?**

`TSC=1` causes `SMC` instructions from EL1 to trap to EL2 instead of going to EL3. KVM uses this to intercept PSCI (Power State Coordination Interface) calls from the guest. The guest thinks it's calling TF-A for power management (CPU on/off, suspend), but KVM handles these calls by managing vCPU lifecycle. This allows KVM to control guest vCPU power states without actual EL3 involvement.

**Q5: What is HCR_EL2.TVM used for?**

`TVM=1` traps all writes to EL1 virtual memory control registers (`SCTLR_EL1`, `TCR_EL1`, `TTBR0/1_EL1`, `MAIR_EL1`, etc.) to EL2. KVM uses this in "shadow page table" mode to track when the guest changes its page tables. When the guest writes a new TTBR0_EL1, KVM is notified (via the trap) and can update its Stage 2 mappings accordingly. In modern KVM, `TVM` is cleared after the initial setup because KVM uses a "lazy" approach to page table synchronization via Stage 2 page faults instead.

---

## 9. Quick Reference

| Bit | Name | Description | KVM Value |
|---|---|---|---|
| 0 | VM | Stage 2 enable | 1 (in guest), 0 (in host) |
| 3 | FMO | FIQ → EL2 | 1 |
| 4 | IMO | IRQ → EL2 | 1 |
| 5 | AMO | SError → EL2 | 1 |
| 6 | VF | Virtual FIQ | Set to inject |
| 7 | VI | Virtual IRQ | Set to inject |
| 13 | TWI | Trap WFI | 1 |
| 14 | TWE | Trap WFE | 1 |
| 19 | TSC | Trap SMC | 1 |
| 26 | TVM | Trap VM register writes | 0 (lazy) |
| 27 | TGE | All EL0 exceptions → EL2 | 1 (host VHE only) |
| 31 | RW | EL1/EL0 = AArch64 | 1 |
| 34 | E2H | VHE enable | 1 (if VHE supported) |
