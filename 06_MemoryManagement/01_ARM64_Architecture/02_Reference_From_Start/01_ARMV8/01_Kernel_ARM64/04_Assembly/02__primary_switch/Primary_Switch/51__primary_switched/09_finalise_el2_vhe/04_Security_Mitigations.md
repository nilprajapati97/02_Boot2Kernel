# `finalise_el2` and Spectre/Security Mitigations

## EL2 Security Boundary

`finalise_el2` establishes the EL2 security boundary that protects the host
kernel and VMs from hypervisor-level attacks:

```
Attack Surface Before finalise_el2:
─────────────────────────────────────
EL2 in indeterminate state
    • HCR_EL2 may have wrong routing bits
    • VBAR_EL2 may point to firmware vectors
    • HSTR_EL2 may not trap required operations
    • MDCR_EL2 may not configure debug correctly
    → Potential for debug register abuse from guest to EL2

After finalise_el2:
─────────────────────────────────────
EL2 in known, controlled state
    • HCR_EL2: only allow what's needed
    • VBAR_EL2: our vectors
    • HSTR_EL2: trap coprocessor access (CP14, CP15)
    • MDCR_EL2: debug controlled by host
    → Attack surface minimized
```

---

## Spectre-v2 and SSBS Mitigations

EL2 plays a crucial role in Spectre/Meltdown mitigations:

```c
// arch/arm64/kernel/head.S (part of finalise_el2 chain):
// Configure MDCR_EL2 for debug/PMU security:
void __init finalise_el2_mdcr(void)
{
    u64 mdcr;
    
    mdcr = read_sysreg(mdcr_el2);
    
    // Disable external debug at EL2 (only EL3/Secure can debug EL2):
    mdcr &= ~MDCR_EL2_TDOSA;    // don't trap OS registers
    mdcr |= MDCR_EL2_TDA;       // DO trap debug access from EL1
    
    // PMU access control (performance monitoring):
    mdcr &= ~MDCR_EL2_HPME;     // disable EL2 PMU events
    
    write_sysreg(mdcr, mdcr_el2);
}
```

**SSBS (Speculative Store Bypass Safe)** — mitigates Spectre variant 4:
```asm
// In finalise_el2_vhe or shortly after:
// Set PSTATE.SSBS = 1 (enable Speculative Store Bypass Safe behavior)
// This tells the CPU to be conservative about speculative loads
msr     ssbs, #1
```

**CSV2/CSV3** (Cache Speculation Variant 2/3):
- Controlled by SCXTNUM_EL0/EL1 (speculation control registers)
- Set up during `finalise_el2` to prevent cross-context speculation attacks

---

## `HSTR_EL2` — Hypervisor System Trap Register

`HSTR_EL2` controls which EL1 coprocessor accesses are trapped to EL2:

```
HSTR_EL2 bits:
Bit 15: T15 — Trap CP15 (the main AArch32 system register coprocessor)
Bit 14: T14 — Trap CP14 (debug coprocessors)
...

For nVHE KVM (after finalise_el2_nvhe):
    HSTR_EL2 = 0  (KVM will configure this when VM starts)
    Later, when VM runs: HSTR_EL2 = appropriate trap bits

Purpose: Prevent guest VM from reading/writing EL1 system registers
that would give it information about the host or allow escape.
```

---

## EL2 Stage-2 Page Tables (S2 MMU)

`finalise_el2` also sets up the foundation for Stage-2 MMU (S2 translation),
which is how VMs are isolated from the host:

```
Without VMs:
    EL1/EL0 access: VA → (EL1 page tables TTBR1_EL1) → PA

With VMs (nVHE):
    Guest EL1/EL0 access: Guest VA → (Stage 1) → IPA → (Stage 2) → PA
                            ↑                              ↑
                       Guest page tables           KVM EL2 S2 tables
                                                   (host controls this)

Stage-2 provides:
- Memory isolation: guest cannot access host memory
- MMIO emulation: guest device register accesses trapped to KVM
- Memory encryption: SPE (pointer auth) integrated
```

`finalise_el2` initializes `VTCR_EL2` (Virtualization Translation Control):
```c
// arch/arm64/kvm/hyp/include/nvhe/mem_protect.h:
VTCR_EL2 configuration:
    T0SZ: Guest IPA address space size
    SL0:  Starting level of Stage-2 page tables
    PS:   Physical address size (matches CPU capability)
    TG0:  Translation granule (4K/16K/64K)
```

---

## Timer and GIC Virtualization

`finalise_el2` also sets up virtual timer and interrupt controller for VMs:

```c
// Timer virtualization (CNTHCTL_EL2):
void finalise_el2_timer(void)
{
    u32 cnthctl = read_sysreg(cnthctl_el2);
    
    // Allow EL1 and EL0 to access physical counter/timer:
    cnthctl |= CNTHCTL_EL1PCEN;    // physical timer enable
    cnthctl |= CNTHCTL_EL1PCTEN;   // physical counter enable
    
    write_sysreg(cnthctl, cnthctl_el2);
    
    // Set virtual timer offset to 0 (host = virtual = physical):
    write_sysreg(0, cntvoff_el2);
}
```

Without proper timer configuration in `finalise_el2`:
- Host kernel's timer interrupts may not fire correctly
- Virtual timers for VMs would have incorrect offsets
- Timer-based scheduling would be unreliable

---

## Security: EL2 as Defense-in-Depth

Modern ARM64 security architecture relies on EL2 being correctly configured:

```
Security layers:
    EL3: TrustZone / Secure Monitor (highest privilege, firmware)
    EL2: Hypervisor (second tier — isolates EL1 kernels)
    EL1: OS kernel (third tier — isolates EL0 processes)
    EL0: User processes (lowest privilege)

finalise_el2 is the kernel's ONE CHANCE to configure EL2 correctly:
    - Any missed configuration = security gap
    - Any overly permissive configuration = attack surface

A correctly configured EL2 (after finalise_el2) ensures:
    1. Guest VMs cannot read host memory
    2. Guest VMs cannot access host system registers
    3. Debug register abuse is trapped to EL2 (not silently allowed)
    4. PMU (performance monitoring) doesn't leak host secrets to guests
    5. Speculative execution attacks are mitigated at EL2 boundary
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
Spectre (variant 1 and 2) and Meltdown (variant 3) exploit speculative execution in out-of-order ARMv8-A cores. Variant 1: bounds check bypass -- the CPU speculatively executes past an array bounds check and leaks data via a cache side-channel. Variant 2: branch target injection -- an attacker trains the indirect branch predictor (IBP) to redirect speculative execution to a gadget. Meltdown: exception handler bypass -- the CPU speculatively reads kernel memory from user context. ARM Cortex-A57/A72/A75 are affected. Cortex-A53/A55 are NOT affected by Meltdown.

### Kernel Perspective (Linux ARM64)
Linux ARM64 mitigations include:
- Spectre-v1: array_index_nospec() barriers in critical syscall paths.
- Spectre-v2: IBRS (Indirect Branch Restricted Speculation) or retpolines, enabled via CONFIG_HARDEN_BRANCH_PREDICTOR. The early boot path in __primary_switch is not directly exposed but the BR x8 at the PA->VA transition is the first indirect branch that could be mispredicted.
- Meltdown: not applicable to ARM64 (no user->kernel speculation of that type).
- Spectre-BHB (v3a): CSV2/CSV3 and BHI_DIS_EL1 mitigations for Branch History Injection, added in Linux 5.17 for Cortex-A78/X1/X2.

### Memory Perspective (ARMv8 Memory Model)
Spectre attacks are memory attacks: the speculative load fills a cache line at an attacker-controlled physical address. The attacker then times a probe load to determine which cache line was filled. The ARMv8 memory model does not define cache timing -- the timing channel is microarchitectural, not architectural. Linux mitigation for Spectre-v1 uses barrier instructions (e.g., CSDB -- Consumption of Speculative Data Barrier, ARMv8.3) to prevent the speculative value from being used as a memory address. CSDB is a new ARMv8.3 instruction that stops speculative memory accesses derived from a value loaded after an mispredicted branch.