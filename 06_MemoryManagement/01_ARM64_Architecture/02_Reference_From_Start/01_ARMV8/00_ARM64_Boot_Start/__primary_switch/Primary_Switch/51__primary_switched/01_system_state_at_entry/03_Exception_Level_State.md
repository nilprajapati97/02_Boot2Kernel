# Exception Level State at Entry — EL Analysis

## What Exception Level Are We At?

**Answer:** Either EL1 or EL2, depending on the platform and bootloader.

This is determined by `init_kernel_el()` in `primary_entry` and recorded in x20.
By the time `__primary_switched` runs, the EL has been PARTIALLY configured but
NOT FINALIZED — `finalise_el2` in `__primary_switched` makes the final EL decision.

---

## Exception Level Hierarchy Review

```
ARM64 Exception Level Model:

EL3  ← Secure Monitor (ATF/TrustZone — manages Secure/Non-Secure worlds)
EL2  ← Hypervisor (KVM host, or hypervisor stub, or VHE kernel)
EL1  ← OS Kernel
EL0  ← User applications

Hardware rule: you can only ERET to a lower or equal EL.
You cannot software-jump to a higher EL — only exceptions can raise EL.
```

---

## Case 1: Booted at EL1 (x20 = BOOT_CPU_MODE_EL1 = 0x1)

**When this happens:**
- Platform EL3 firmware dropped to EL1 (common on emulators like QEMU TCG)
- EFI runtime services drop to EL1 before calling kernel
- Test environments without hypervisor

**State at `__primary_switched` entry:**
```
CurrentEL = EL1 (bits [3:2] = 0b01)
SCTLR_EL1 = configured (M=1, C=1, I=1)
HCR_EL2 = NOT accessible (EL1 cannot access EL2 registers)
VBAR_EL1 = 0 (not yet set)
```

**What `finalise_el2` does in this case:**
- `x0 = BOOT_CPU_MODE_EL1` → function detects EL1
- Does nothing (or minimal EL1 setup)
- Returns to EL1 execution — no mode change

**KVM implications:** With EL1 boot, KVM cannot be used (requires EL2 for hypervisor
stage-2 page tables). `is_hyp_mode_available()` returns false.

---

## Case 2: Booted at EL2 Without VHE (x20 = BOOT_CPU_MODE_EL2, E2H=0)

**When this happens:**
- Most physical ARM64 SoCs (Qualcomm, MediaTek, Samsung Exynos, NVIDIA Tegra)
- Bootloader/ATF hands off at EL2 without enabling VHE
- ARM Cortex-A55/A75/A76/A77/A78 without VHE configured

**State at `__primary_switched` entry:**
```
CurrentEL = EL2 (bits [3:2] = 0b10)
HCR_EL2.M = 1    (EL2 MMU on — we're running AT EL2 right now)
HCR_EL2.E2H = 0  (VHE NOT enabled)
VBAR_EL2 = &__hyp_stub_vectors (set by init_el2 in init_kernel_el)
VBAR_EL1 = 0 (not yet set — for when we drop to EL1)
```

**What `finalise_el2` does:**
```asm
// Detects: x0 = BOOT_CPU_MODE_EL2, HCR_EL2.E2H = 0
// Sets up SPSR_EL2 for EL1 target
// Executes ERET to drop from EL2 to EL1
// Kernel continues at EL1 from this point
```

**Post-`finalise_el2`:** CurrentEL = EL1. KVM can use EL2 for stage-2 tables.

---

## Case 3: Booted at EL2 With VHE (x20 = BOOT_CPU_MODE_EL2 | BOOT_CPU_FLAG_E2H)

**When this happens:**
- ARM Cortex-A76 and newer with VHE explicitly enabled
- ARMv8.1+ hardware + bootloader that sets HCR_EL2.E2H=1
- NVIDIA Orin, Qualcomm Snapdragon 8cx Gen 3 and newer
- Apple M-series chips (M1/M2 run at EL2 with VHE)

**State at `__primary_switched` entry:**
```
CurrentEL = EL2 (bits [3:2] = 0b10)
HCR_EL2.E2H = 1   (VHE ACTIVE)
HCR_EL2.TGE = 0   (kernel is host — not in guest mode)
HCR_EL2.RW = 1    (lower EL is AArch64)
```

**Key VHE property:** With E2H=1, EL1 system register accesses are ALIASED to EL2:
```
msr sctlr_el1, x0   →  actually writes SCTLR_EL2
msr ttbr0_el1, x0   →  actually writes TTBR0_EL2
msr vbar_el1, x0    →  actually writes VBAR_EL2
```
The kernel code is UNAWARE it is running at EL2 — hardware aliases make it transparent.

**What `finalise_el2` does:**
- Detects E2H=1 in x20 flags
- Keeps HCR_EL2.E2H=1 active
- Does NOT ERET to EL1 — stays at EL2
- Returns normally (BL → BLR → RET)

**Post-`finalise_el2`:** Still at EL2. `is_kernel_in_hyp_mode()` returns true.
KVM operations are much faster (no EL2↔EL1 world switch needed).

---

## VBAR_EL1 — The Critical UNSAFE Register

**Value at entry:** 0 or firmware garbage

**ARM64 behavior when exception fires with VBAR_EL1 = 0:**
```
Any exception at EL1 (synchronous, IRQ, FIQ, SError):
  handler_addr = VBAR_EL1 + offset
              = 0x0000_0000_0000_0000 + 0x200  (for sync EL1, SP_ELx)
              = 0x0000_0000_0000_0200

Virtual address 0x200:
  → TTBR1 handles VA[63]=1 range — 0x200 is TTBR0 range
  → TTBR0 identity map: maps PA to same VA in low physical range
  → PA 0x200 = physical byte 512 = likely in ROM or undefined memory
  → Not a valid exception handler
  → CPU takes ANOTHER translation fault
  → Loops → system hang
```

**The 2-instruction danger window:**
Between `br x8` (entering `__primary_switched`) and `msr vbar_el1, x8` (installing
valid vectors), there is a window where any exception = instant silent hang.

This window is intentionally minimized by running `init_cpu_task` first (which needs
a valid SP for exception entry to be safe) and immediately running `msr vbar_el1`.

**Interrupts:** IRQs are masked (`DAIF.I = 1`) at EL1 entry from EL2 via `ERET` in
`init_kernel_el`. This reduces (but does not eliminate) the exception risk during this window.
NMI (SError, Debug), data aborts from speculative accesses, and instruction faults
can still fire even with IRQs masked.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.