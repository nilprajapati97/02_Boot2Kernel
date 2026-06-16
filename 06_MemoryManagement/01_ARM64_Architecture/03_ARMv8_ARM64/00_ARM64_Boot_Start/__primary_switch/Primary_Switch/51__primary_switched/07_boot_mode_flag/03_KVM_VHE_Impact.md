# KVM and VHE — Impact of `__boot_cpu_mode` on KVM Initialization

## KVM Architecture Selection at Boot

```c
// arch/arm64/kvm/arm.c
int kvm_arch_init(void *opaque)
{
    int err;
    
    if (!is_hyp_mode_available()) {
        kvm_info("HYP mode not available\n");
        return -ENODEV;
    }
    
    if (is_kernel_in_hyp_mode()) {
        // VHE mode: kernel is at EL2
        kvm_info("VHE mode initialized successfully\n");
        err = kvm_arm_init_hyp_services();
    } else {
        // nVHE mode: kernel at EL1, hypervisor at EL2
        kvm_info("Non-VHE mode initialized successfully\n");
        err = kvm_arm_init_nvhe();
    }
    ...
}
```

`is_hyp_mode_available()`: checks `__boot_cpu_mode[0] == BOOT_CPU_MODE_EL2`
`is_kernel_in_hyp_mode()`: checks `HCR_EL2.E2H == 1` (VHE active)

---

## Performance Comparison: nVHE vs VHE

When a VM exits (guest code does a hypervisor-trapped operation):

**nVHE exit (pre-VHE):**
```
Guest (EL1)
    ↓ trap → EL2
EL2: Save ALL EL1 registers (SP_EL1, ELR_EL1, SPSR_EL1, SCTLR_EL1, ...)
     Restore KVM EL2 registers
     Handle VM exit in EL2
     Save KVM EL2 registers
     Restore EL1 host registers
    ↓ eret → EL1
Host kernel handles I/O, interrupt, etc.
    ↓ hvc → EL2
EL2: Similar save/restore for guest entry
    ↓ eret → EL1 (guest)

Cost: ~1000-2000 cycles of register save/restore overhead per VM exit
```

**VHE exit (post-ARMv8.1):**
```
Guest (EL1, redirected)
    ↓ trap → EL2 (= host EL1 in VHE)
Host kernel at EL2 handles VM exit DIRECTLY
    ↓ eret → EL1 (guest, redirected)

Cost: ~100-200 cycles (no full register context switch)
```

VHE reduces VM exit overhead by ~10×. This is why `__boot_cpu_mode` and VHE
detection are important for KVM performance.

---

## How `finalise_el2` Decides VHE vs nVHE

After `set_cpu_boot_mode_flag` in `__primary_switched`:
```asm
mov     x0, x20              // x0 = boot mode
bl      finalise_el2         // configure EL2 (or do nothing for EL1 boot)
```

```asm
// arch/arm64/kernel/head.S finalise_el2:
SYM_FUNC_START(finalise_el2)
    cbnz    x0, 1f           // skip if not EL2 boot
    ret                      // EL1 boot: nothing to do
1:
    // EL2 boot: configure for VHE or nVHE
    mrs     x0, hcr_el2
    and     x0, x0, #HCR_E2H  // check VHE flag
    cbz     x0, 2f           // if E2H=0: nVHE mode
    
    // VHE mode setup:
    msr     spsr_el2, x19    // set up SPSR for VHE entry
    bl      finalise_el2_vhe
    b       3f
    
2:  // nVHE mode setup:
    bl      finalise_el2_nvhe
    
3:  ret
SYM_FUNC_END(finalise_el2)
```

The `finalise_el2` decision tree uses `x20` (boot mode from `init_kernel_el`)
as a quick EL1/EL2 check, then reads `HCR_EL2.E2H` to determine VHE vs nVHE.

---

## Practical Impact for Production

**Mobile/embedded SoCs (Qualcomm Snapdragon, MediaTek):**
- Usually boot at EL2 for TrustZone support
- Android runs KVM in nVHE mode (VMs are Android containers)
- `__boot_cpu_mode = BOOT_CPU_MODE_EL2` → KVM available → Android virtualization

**Server SoCs (Ampere Altra, AWS Graviton, Neoverse N2):**
- Always boot at EL2 (ACPI SBBR compliant)
- VHE mode for KVM (ARMv8.1+ CPUs)
- `__boot_cpu_mode = BOOT_CPU_MODE_EL2` → VHE KVM → high-performance VMs

**Embedded/RTOS-adjacent boards (Raspberry Pi):**
- May boot at EL1 (depends on firmware configuration)
- `__boot_cpu_mode = BOOT_CPU_MODE_EL1` → KVM unavailable
- `is_hyp_mode_available()` returns false → no KVM support

---

## Checking Boot Mode on a Running System

```bash
# Check if KVM is available (proxy for EL2 boot):
ls /dev/kvm
# If exists: booted at EL2, KVM available

# Check VHE vs nVHE from kernel messages:
dmesg | grep -i "vhe\|nvhe\|hyp"
# VHE: "KVM: VHE mode initialized successfully"
# nVHE: "KVM: Non-VHE mode initialized successfully"

# Check actual HCR_EL2 at runtime (requires EL2 access):
# Only possible from EL2 code (KVM hypervisor call)
# From userspace: cat /sys/kernel/debug/kvm/...
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.