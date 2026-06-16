# ARM64 Exception Levels — Deep Architecture Reference

## The Four Exception Levels

ARM64 implements a privilege hierarchy via Exception Levels:

```
EL3 ─── Secure Monitor ──── ARM TrustZone (highest privilege)
  │      (ATF/BL31)           - Controls S/NS world switching
  │                           - SMC (Secure Monitor Call) handler
  │
EL2 ─── Hypervisor ──────── KVM, Xen (second highest)
  │      (KVM running here)   - Stage 2 page tables (IPA → PA)
  │                           - CPU virtualization (VPIDR, VMPIDR tricks)
  │                           - Virtual interrupt injection
  │
EL1 ─── OS Kernel ───────── Linux kernel (EL1)
  │      (Linux)              - Stage 1 page tables (VA → IPA/PA)
  │                           - Kernel/user mode via PSTATE.EL0/EL1
  │
EL0 ─── User Space ─────── Applications
         (glibc, apps)        - Least privilege
                              - Cannot access kernel registers
```

Transitions between ELs:
- EL0 → EL1: `svc` instruction (system call)
- EL0/EL1 → EL2: `hvc` instruction (hypervisor call)
- EL0/EL1/EL2 → EL3: `smc` instruction (secure monitor call)
- Down: `eret` instruction (exception return)

---

## CurrentEL Register

```asm
mrs  x0, CurrentEL
```

`CurrentEL` is a read-only system register. Bits [3:2] encode the current EL:
- `0b00` (0): EL0
- `0b01` (4): EL1
- `0b10` (8): EL2
- `0b11` (12): EL3

Linux `init_kernel_el` reads this to determine whether to stay at EL2 or configure
and drop to EL1:
```asm
mrs     x0, CurrentEL
cmp     x0, #CurrentEL_EL2   // #CurrentEL_EL2 = 8
b.eq    1f                    // branch if EL2
// Code for EL1 entry...
b       2f
1:  // Code for EL2 configuration (VHE setup, etc.)...
2:
```

---

## VHE — Virtualization Host Extensions

VHE is an ARMv8.1-A feature that allows the Linux kernel to run directly at EL2
(instead of EL1) when acting as a host hypervisor with KVM.

Benefits:
1. Eliminates EL1↔EL2 switching overhead (saves ~1000 cycles per VM exit)
2. Allows KVM guests at EL1, host Linux at EL2 natively
3. EL2 has direct access to EL2 system registers (no save/restore needed)

When VHE is active:
- `HCR_EL2.E2H=1`: host applications run at EL0 using EL2 page tables
- `HCR_EL2.TGE=1`: host traps go to EL2 (not EL1)
- The kernel appears to run at EL1 (compatible with EL1 code) but actually runs at EL2

The `__boot_cpu_mode` flag distinguishes:
- `BOOT_CPU_MODE_EL1`: no VHE (booted at EL1, no hypervisor access)
- `BOOT_CPU_MODE_EL2`: may use VHE (booted at EL2)

---

## nVHE vs VHE Modes for KVM

When `__boot_cpu_mode = BOOT_CPU_MODE_EL2`:

**nVHE mode** (non-VHE, pre-ARMv8.1 or forced):
```
EL2: KVM hypervisor stub
EL1: Linux kernel
EL0: user processes
     + VM guests (at EL1 via Stage 2 tables)
```

**VHE mode** (ARMv8.1+, `HCR_EL2.E2H=1`):
```
EL2 (acting as EL1): Linux kernel + KVM
EL0 (acting as EL0): user processes
EL1: VM guests (EL1-redirected)
EL0 (VM): VM user processes
```

The decision between nVHE and VHE is made in `finalise_el2` (the next step
after `set_cpu_boot_mode_flag` in `__primary_switched`).

---

## BOOT_CPU_MODE Values in the Kernel Source

```c
// arch/arm64/include/asm/virt.h
#define BOOT_CPU_MODE_EL1   (0x0e11)  // 3601 decimal — mnemonic: EL1
#define BOOT_CPU_MODE_EL2   (0x0e12)  // 3602 decimal — mnemonic: EL2
```

The values `0x0e11` and `0x0e12` are chosen to be distinctive (easy to identify
in a hex dump) and to NOT be valid instruction encodings (reducing false positives
in memory analysis).

---

## Secondary CPU Boot Mode Verification

```c
// arch/arm64/kernel/smp.c
void __init smp_prepare_cpus(unsigned int max_cpus)
{
    ...
}

// arch/arm64/kernel/head.S (secondary_startup path):
// Each secondary CPU also calls set_cpu_boot_mode_flag:
secondary_startup:
    ...
    bl  set_cpu_boot_mode_flag   // same call as primary
    ...

// Consistency check after all CPUs up:
void __init check_cpu_features(void)
{
    if (__boot_cpu_mode[0] != __boot_cpu_mode[1]) {
        pr_err("FATAL: CPUs booted in different modes: "
               "CPU0=%s, CPU%d=%s\n", ...);
        panic("Inconsistent CPU boot modes");
    }
}
```

If some CPUs boot at EL2 and others at EL1, the kernel panics. This is a
critical consistency requirement — all CPUs must see the same virtualization
environment.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.