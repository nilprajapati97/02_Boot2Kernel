# Hypervisor Support (EL2)

## 1. EL2 — The Hypervisor Exception Level

EL2 is a dedicated privilege level for hypervisors. It sits between EL1 (OS kernel) and EL3 (Secure Monitor) and has controls to trap, intercept, and emulate guest operations.

```
Privilege hierarchy:
  EL3 ─── Highest (Secure Monitor)
   │
  EL2 ─── Hypervisor (controls VMs)
   │
  EL1 ─── Guest OS kernel (thinks it's in charge)
   │
  EL0 ─── Guest applications

Key concept: "Trap and Emulate"
  Guest executes privileged instruction at EL1
  → Hardware traps to EL2 (hypervisor)
  → Hypervisor emulates the instruction
  → Returns to guest at EL1
  
  This is transparent to the guest — it doesn't know
  it's running in a VM.
```

---

## 2. HCR_EL2 — Hypervisor Configuration Register

HCR_EL2 is the most important virtualization register. It controls what gets trapped to EL2.

```
HCR_EL2 Key Fields:
┌─────┬────────┬────────────────────────────────────────────────┐
│ Bit │ Name   │ Function                                       │
├─────┼────────┼────────────────────────────────────────────────┤
│  0  │ VM     │ 1=Enable Stage-2 translation (IPA→PA)         │
│  1  │ SWIO   │ 1=Set/Way cache ops generate trap (for coherent│
│     │        │   guests that try to clean cache by set/way)   │
│  3  │ FMO    │ 1=Route physical FIQ to EL2                   │
│  4  │ IMO    │ 1=Route physical IRQ to EL2                   │
│  5  │ AMO    │ 1=Route SError to EL2                         │
│  7  │ VI     │ 1=Assert virtual IRQ to guest                 │
│  6  │ VF     │ 1=Assert virtual FIQ to guest                 │
│ 12  │ TSW    │ 1=Trap set/way cache maintenance to EL2       │
│ 13  │ TWI    │ 1=Trap WFI to EL2                             │
│ 14  │ TWE    │ 1=Trap WFE to EL2                             │
│ 19  │ TSC    │ 1=Trap SMC to EL2 (intercept guest SMC calls) │
│ 20  │ TID3   │ 1=Trap reads of ID registers to EL2           │
│ 26  │ TVM    │ 1=Trap writes to virtual memory registers     │
│ 27  │ TGE    │ 1=Trap General Exceptions (EL0 → EL2)        │
│ 30  │ TRVM   │ 1=Trap reads of virtual memory registers      │
│ 31  │ RW     │ 1=EL1 is AArch64, 0=EL1 is AArch32           │
│ 34  │ E2H    │ 1=Enable VHE (EL2 host mode)                  │
│ 35  │ TLOR   │ 1=Trap LOR registers                         │
│ 38  │ MIOCNCE│ 1=Mismatched Inner/Outer Cacheable Non-Combine│
│ 40  │ APK    │ 1=Trap PAC key registers                      │
│ 41  │ API    │ 1=Trap PAC instructions                       │
│ 45  │ FWB    │ 1=Force Write-Back (ARMv8.4)                  │
│ 46  │ NV     │ 1=Nested Virtualization (ARMv8.3)             │
└─────┴────────┴────────────────────────────────────────────────┘

Typical KVM configuration:
  HCR_EL2 = VM | SWIO | FMO | IMO | AMO | TSC | RW
  • VM=1: Stage-2 translation active
  • FMO/IMO: Physical interrupts trapped to EL2
  • TSC: Guest SMC trapped to hypervisor
  • RW=1: Guest runs AArch64
```

---

## 3. Trap and Emulate — How It Works

```
Example: Guest reads MIDR_EL1 (CPU ID register)

  Guest code:
    MRS X0, MIDR_EL1         // Read CPU ID
  
  With HCR_EL2.TID3=1:
    1. Hardware detects trapped register access
    2. Exception taken to EL2 (ESR_EL2 records the details)
    3. Hypervisor handler:
       - ESR_EL2.EC = 0x18 (MSR/MRS trap)
       - ESR_EL2.ISS tells which register: MIDR_EL1
       - ESR_EL2.ISS.Rt tells destination: X0
    4. Hypervisor decides what value to return:
       - Real value: just read MIDR_EL1 and forward
       - Fake value: return a different CPU ID (e.g., hide
         specific core revision from guest)
    5. Write result to guest's X0 (via ELR_EL2 context)
    6. Advance ELR_EL2 by 4 (skip the trapped instruction)
    7. ERET back to guest

  Guest sees: X0 = CPU ID value (doesn't know it was emulated)

Example: Guest writes to SCTLR_EL1 (system control)

  With HCR_EL2.TVM=1:
    1. Guest: MSR SCTLR_EL1, X0
    2. Trap to EL2
    3. Hypervisor:
       - Shadow the register (keep track of guest's view)
       - Apply only safe settings to real SCTLR_EL1
       - Prevent guest from disabling MMU (needed for Stage-2)
    4. ERET back to guest
```

---

## 4. VHE — Virtualization Host Extensions (ARMv8.1)

```
Problem: Type-1 hypervisors (KVM) run host OS at EL1.
When a VM traps to EL2, the hypervisor handles it at EL2.
But the host kernel runs at EL1. Context switch between
EL1 (host) and EL2 (hypervisor) is expensive.

Solution: VHE allows the HOST kernel to run at EL2 directly.

Without VHE:                    With VHE (E2H=1, TGE=1):
  EL2: KVM hypervisor code        EL2: Host Linux kernel
  EL1: Host Linux kernel               + KVM hypervisor
  EL0: Host userspace              EL0: Host userspace

  VM entry: EL2→EL1               VM entry: just switch
  VM exit:  EL1→EL2               context at EL2

Register redirection with VHE:
  When HCR_EL2.E2H=1, accesses to EL1 registers are
  redirected to EL2 registers transparently:

  ┌──────────────────────┬──────────────────────────┐
  │ Instruction          │ Actual register accessed │
  ├──────────────────────┼──────────────────────────┤
  │ MRS X0, SCTLR_EL1   │ Reads SCTLR_EL2          │
  │ MRS X0, TTBR0_EL1   │ Reads TTBR0_EL2          │
  │ MRS X0, TCR_EL1     │ Reads TCR_EL2            │
  │ MRS X0, VBAR_EL1    │ Reads VBAR_EL2           │
  │ MRS X0, MAIR_EL1    │ Reads MAIR_EL2           │
  │ MRS X0, SPSR_EL1    │ Reads SPSR_EL2           │
  │ MRS X0, ELR_EL1     │ Reads ELR_EL2            │
  └──────────────────────┴──────────────────────────┘

  Benefit: Host kernel code doesn't need modification!
  It uses EL1 register names, but hardware redirects to EL2.
  Linux: CONFIG_ARM64_VHE — enabled by default

Guest entry with VHE:
  1. Clear TGE (HCR_EL2.TGE=0) — guest mode
  2. Set VM=1, load guest's Stage-2 tables
  3. Load guest context (registers, saved state)
  4. ERET to guest EL1
  
Guest exit:
  1. Trap/exception arrives at EL2
  2. Set TGE=1 — back to host mode
  3. Save guest context
  4. Handle exit reason
```

---

## 5. Virtual Interrupts

```
Hypervisor injects virtual interrupts into guests:

Method 1: Software injection via HCR_EL2.VI/VF
  Set HCR_EL2.VI = 1 → guest sees pending virtual IRQ
  Guest takes IRQ exception, handler reads virtual IAR
  Simple but can only inject one interrupt at a time

Method 2: GICv3 List Registers (LRs)
  ICH_LR<n>_EL2 (n = 0-15): each LR holds one virtual interrupt

  ICH_LR_EL2 format:
  ┌─────────────────────────────────────────────────────────┐
  │ [63:62] State: 00=invalid, 01=pending, 10=active,      │
  │                 11=pending+active                       │
  │ [61]    HW: 1=backed by physical interrupt (deactivate │
  │             physical when guest EOIs virtual)           │
  │ [60]    Group: 0=Group 0 (FIQ), 1=Group 1 (IRQ)       │
  │ [55:48] Priority                                        │
  │ [44:32] Physical INTID (when HW=1)                     │
  │ [31:0]  Virtual INTID                                   │
  └─────────────────────────────────────────────────────────┘

  Interrupt injection flow:
  1. Physical device IRQ → EL2 (hypervisor)
  2. Hypervisor determines which VM owns this device
  3. Write ICH_LR with virtual INTID, set state=pending
  4. ERET to guest
  5. Guest sees IRQ, reads GICC_IAR → gets virtual INTID
  6. Guest handles interrupt, writes GICC_EOIR
  7. If HW=1: physical interrupt also deactivated
  8. If HW=0: maintenance interrupt tells hypervisor EOI done

Method 3: GICv4 Direct Injection (fastest)
  Physical device → ITS → directly to vPE (guest VCPU)
  No hypervisor trap needed!
  (See Interrupt Subsystem docs for details)
```

---

## 6. Virtual Timer

```
Each core has multiple timers. The hypervisor manages them:

  ┌────────────────────────────────────────────────────────┐
  │  Timer              │ Purpose                          │
  ├─────────────────────┼──────────────────────────────────┤
  │ CNTPCT_EL0          │ Physical counter (always running)│
  │ CNTP_* (Physical)   │ EL1 physical timer               │
  │ CNTV_* (Virtual)    │ EL1 virtual timer (for guests)  │
  │ CNTHP_* (Hyp Phys)  │ EL2 physical timer              │
  │ CNTHV_* (Hyp Virt)  │ EL2 virtual timer (ARMv8.1)    │
  └─────────────────────┴──────────────────────────────────┘

Virtual Timer for Guests:
  CNTVOFF_EL2: Virtual counter offset (set by hypervisor)
  
  Guest reads CNTVCT_EL0 = CNTPCT_EL0 - CNTVOFF_EL2
  
  This lets hypervisor adjust virtual time:
  • When guest is preempted → add time delta to offset
  • Guest sees continuous time progression
  
  Physical timer: trapped to EL2 (guest can't use it directly)
  Virtual timer: guest uses freely (fires PPI 27)
```

---

## 7. Nested Virtualization (ARMv8.3-NV)

```
Nested virtualization allows running a hypervisor inside a VM:

  ┌─────────────────────────────────────────────────┐
  │  EL2: L0 Hypervisor (real hardware)             │
  │  ┌─────────────────────────────────────────┐    │
  │  │  VM (Guest):                             │    │
  │  │  EL1: L1 Hypervisor (nested, in VM)     │    │
  │  │  ┌─────────────────────────────────┐    │    │
  │  │  │  Nested VM:                      │    │    │
  │  │  │  EL0/EL1: Guest OS in nested VM │    │    │
  │  │  └─────────────────────────────────┘    │    │
  │  └─────────────────────────────────────────┘    │
  └─────────────────────────────────────────────────┘

How it works:
  • HCR_EL2.NV=1: L1 hypervisor's EL2 accesses are trapped to L0
  • L0 hypervisor emulates EL2 for L1 hypervisor
  • VNCR_EL2 register: points to memory page where L1's
    virtual EL2 system registers are stored
  
  Use case: Running KVM inside a KVM VM (cloud migration testing)
```

---

## 8. VMID and Context Switching

```
VMID (Virtual Machine Identifier):
  • 8-bit (256 VMs) or 16-bit (65536 VMs, ARMv8.1 VMID16)
  • Stored in VTTBR_EL2[63:48]
  • Tags TLB entries so entries from different VMs don't mix

VM Context Switch (KVM):
  Hypervisor saves/restores these per-VCPU:
  ┌─────────────────────────────────────────────────┐
  │ Category           │ Registers                   │
  ├────────────────────┼─────────────────────────────┤
  │ General purpose    │ X0-X30, SP_EL0, SP_EL1     │
  │ System registers   │ SCTLR_EL1, TCR_EL1,        │
  │                    │ TTBR0/1_EL1, MAIR_EL1,     │
  │                    │ VBAR_EL1, CONTEXTIDR_EL1,  │
  │                    │ CPACR_EL1, ESR_EL1,        │
  │                    │ FAR_EL1, PAR_EL1, TPIDR_EL1│
  │ Floating-point     │ V0-V31, FPCR, FPSR         │
  │ Timer              │ CNTVOFF_EL2, CNTV_CVAL/CTL │
  │ GIC state          │ ICH_LR<0-15>_EL2,          │
  │                    │ ICH_VMCR_EL2               │
  │ Debug              │ MDSCR_EL1, breakpoints,     │
  │                    │ watchpoints                  │
  │ Stage-2            │ VTTBR_EL2 (new VM's tables) │
  │ HCR_EL2           │ Per-VM trap configuration    │
  └────────────────────┴─────────────────────────────┘
  
  KVM uses: ~100+ system registers saved/restored per VM switch
  VHE reduces this by eliminating EL1↔EL2 transitions for host
```

---

Next: [Stage-2 Translation →](./02_Stage2_Translation.md) | Back to [Virtualization Overview](./README.md)
