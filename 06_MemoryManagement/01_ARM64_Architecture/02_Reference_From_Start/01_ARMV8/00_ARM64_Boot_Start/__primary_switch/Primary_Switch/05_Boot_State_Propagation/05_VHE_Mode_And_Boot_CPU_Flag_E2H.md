# VHE Mode and `BOOT_CPU_FLAG_E2H` — Complete Analysis

**Register:** `x20` bit 32 (`BOOT_CPU_FLAG_E2H = BIT_ULL(32)`)  
**Source:** `arch/arm64/include/asm/virt.h` line 62  
**Context:** When the boot CPU activates VHE (Virtualization Host Extensions)

---

## 0. What Is VHE?

VHE (Virtualization Host Extensions) is an ARM architecture feature introduced
in ARMv8.1. It allows the host OS kernel to run at **EL2** instead of EL1,
with the hypervisor (KVM) merged into the host kernel.

```
Without VHE (ARMv8.0):               With VHE (ARMv8.1+):
┌────────────────┐                   ┌────────────────┐
│ EL3: TF-A      │                   │ EL3: TF-A      │
├────────────────┤                   ├────────────────┤
│ EL2: KVM       │                   │ EL2: Linux + KVM│ ← Linux runs HERE
├────────────────┤                   ├────────────────┤
│ EL1: Linux     │                   │ EL1: VMs       │
├────────────────┤                   ├────────────────┤
│ EL0: userspace │                   │ EL0: userspace │
└────────────────┘                   └────────────────┘
```

---

## 1. The `HCR_EL2.E2H` Bit — The Hardware Switch

VHE is activated by setting `HCR_EL2.E2H = 1` while at EL2:

```asm
// arch/arm64/kernel/head.S — el2_setup / init_kernel_el:
mrs     x0, hcr_el2
tst     x0, #HCR_E2H               // Test if E2H bit is already set
b.ne    1f                          // Already set? (UEFI left it on)

// Enable VHE if supported:
mrs     x0, id_aa64mmfr1_el1
ubfx    x0, x0, #8, 4              // Check VH field
cbz     x0, no_vhe                 // VH=0 → VHE not supported

// VHE supported and we're at EL2:
mrs     x0, hcr_el2
orr     x0, x0, #HCR_E2H           // Set E2H bit
msr     hcr_el2, x0
isb
```

### What `HCR_EL2.E2H = 1` Does

1. **Redirects EL1 system registers to EL2 counterparts:**
   - `msr sctlr_el1, x0` actually writes `SCTLR_EL2`
   - `msr tcr_el1, x0` actually writes `TCR_EL2`
   - `msr ttbr0_el1, x0` writes `TTBR0_EL2`
   - etc. — transparent to Linux

2. **Enables the `EL0` registers for guest OS:**
   - When running a VM (guest) at EL1, the guest's `msr sctlr_el1` accesses the
     real `SCTLR_EL1` (not EL2) — standard behavior
   - When in VHE host mode (EL2), `sctlr_el1` transparently accesses `SCTLR_EL2`

3. **Enables `HCR_EL2.TGE` for EL0 trapping** (when set alongside E2H):
   - EL0 accesses are trapped to EL2 directly (no intermediate EL1 handler)
   - Reduces context switch overhead for KVM

---

## 2. How `BOOT_CPU_FLAG_E2H` Is Set in x20

```asm
// arch/arm64/kernel/head.S — el2_setup or init_kernel_el:

// If VHE is activated:
mrs     x0, hcr_el2
tst     x0, #HCR_E2H
b.eq    1f                          // Not VHE

orr     x20, x20, #BOOT_CPU_FLAG_E2H  // Set bit 32 in x20
// x20 = BOOT_CPU_MODE_EL2 | BOOT_CPU_FLAG_E2H = 0x1_0000_0E12
1:
```

The `orr` operation uses bit 32 specifically because:
- Bits [15:0] of x20 hold the EL mode value (0xE11 or 0xE12)
- Bits [63:16] are available for flags
- Bit 32 is in the UPPER 32-bit half — cleanly separated from the mode value

---

## 3. How the Kernel Uses `BOOT_CPU_FLAG_E2H`

### In `__primary_switched`:

```asm
// arch/arm64/kernel/head.S — __primary_switched:
tst     x20, #BOOT_CPU_FLAG_E2H    // Test if VHE is active
b.eq    1f                          // No VHE — skip

// VHE is active: the kernel is running at EL2
// Set up EL2-specific registers
...
1:
str     x20, [x4]                   // Store to __boot_cpu_mode
```

### In `is_kernel_in_hyp_mode()`:

```c
static inline bool is_kernel_in_hyp_mode(void)
{
    return read_sysreg(CurrentEL) == CurrentEL_EL2;
}
```

Note: This doesn't use `__boot_cpu_mode` directly. It reads the hardware
`CurrentEL` register which always reflects the current exception level.

### In `kvm_arch_init()`:

```c
// arch/arm64/kvm/arm.c
if (is_kernel_in_hyp_mode()) {
    // VHE: kernel is at EL2, KVM is integrated
    kvm_host_cpu_state = per_cpu_ptr(&kvm_host_data, cpu);
    ...
}
```

### In `nVHE path`:

When `BOOT_CPU_FLAG_E2H` is NOT set:
- The kernel runs at EL1
- KVM uses the **nVHE** (non-VHE) code path (`arch/arm64/kvm/hyp/nvhe/`)
- KVM switches to EL2 only when needed (for VM entry/exit)

---

## 4. EL2 System Register Aliasing With VHE

With `HCR_EL2.E2H = 1`, the following EL1 register accesses are **transparently
aliased** to EL2 registers:

| Linux code uses | Hardware accesses | Note |
|---|---|---|
| `SCTLR_EL1` | `SCTLR_EL2` | VHE-aware |
| `TCR_EL1` | `TCR_EL2` | VHE-aware |
| `TTBR0_EL1` | `TTBR0_EL2` | VHE-aware |
| `TTBR1_EL1` | `TTBR1_EL2` | VHE-aware |
| `MAIR_EL1` | `MAIR_EL2` | VHE-aware |
| `VBAR_EL1` | `VBAR_EL2` | VHE-aware |
| `SP_EL0` | `SP_EL0` | unchanged |
| `SP_EL1` | `SP_EL2` | VHE-aware |

**This is why Linux kernel code doesn't need VHE-specific register names.** The
`__cpu_setup` and `__enable_mmu` code writes `sctlr_el1`, `tcr_el1`, etc., and
with VHE enabled, these transparently reach the EL2 registers. The kernel is
oblivious to whether it's at EL1 or EL2 — the hardware provides the illusion.

---

## 5. Performance Impact of VHE

### Without VHE (nVHE):
```
KVM_ENTER_GUEST:
    ERET from EL1 → EL2 transition (complex state save)
    → run guest at EL1
    HVC from EL1 → EL2 (exit)
    ERET EL2 → EL1 (return to host Linux)
```

Context switch overhead: ~1000-2000 cycles per VM entry/exit.

### With VHE:
```
KVM_ENTER_GUEST:
    ERET from EL2 → EL1 (simple — Linux stays at EL2, guest goes to EL1)
    → run guest at EL1
    HVC from EL1 → EL2 (exit — lands in Linux/KVM at EL2)
```

Context switch overhead: ~500-800 cycles per VM entry/exit.

VHE provides roughly 2× better VM entry/exit performance, which matters for
high-VM-density workloads (like NVIDIA cloud GPU VMs).

---

## 6. Detecting VHE in `/proc/cpuinfo` and `dmesg`

At boot, the kernel logs VHE status:

```
[    0.000000] Kernel/User page tables isolation: disabled (not needed on VHE)
[    0.000000] kvm: Hyp mode initialized successfully
```

With VHE, KPTI (Kernel Page Table Isolation — the Meltdown mitigation) is
not needed because the kernel is at EL2, which is architecturally isolated
from user space (EL0) independently.

```bash
# Check if running with VHE:
$ grep -i vhe /proc/cpuinfo
# or check:
$ dmesg | grep -i vhe
```

---

## 7. VHE and the BOOT_CPU_FLAG_E2H Summary

```
BOOT_CPU_FLAG_E2H in x20:

  Set when:
    1. CPU supports VHE (ID_AA64MMFR1_EL1.VH != 0)
    2. CPU was in EL2 at boot (el2_setup confirmed EL2)
    3. Kernel wants VHE (CONFIG_ARM64_VHE=y or runtime decision)

  Not set when:
    1. CPU started at EL1 (cannot use VHE)
    2. CPU doesn't support VHE (ARMv8.0 only)
    3. VHE explicitly disabled (arm64.novhe command line option)
    4. Firmware didn't leave CPU in EL2

  Effect:
    Set:   Kernel runs at EL2, KVM uses VHE path, EL1 regs alias EL2
    Unset: Kernel runs at EL1, KVM uses nVHE path, EL1/EL2 are distinct
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.