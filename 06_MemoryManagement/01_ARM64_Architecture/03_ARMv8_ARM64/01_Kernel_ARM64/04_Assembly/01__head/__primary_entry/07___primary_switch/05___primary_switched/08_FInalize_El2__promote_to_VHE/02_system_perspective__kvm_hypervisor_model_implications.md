# finalise_el2 — System Perspective: KVM Architecture and Hypervisor Model Implications

**Classification**: ARM64 Virtualization — KVM Architecture Design
**Scope**: Impact of VHE/nVHE selection on KVM, pKVM, and guest execution
**Perspective**: Hypervisor architecture, security model, performance model
**Style Reference**: Google Android Hypervisor / NVIDIA vGPU / AMD-V Architecture

---

## 1. The Three KVM Modes on ARM64

The outcome of `finalise_el2` determines which of three KVM hypervisor
modes the system uses:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    ARM64 KVM Hypervisor Mode Selection                      │
├───────────────────┬──────────────────────────────────────────────────────── │
│  Mode             │  Trigger Condition                                      │
├───────────────────┼──────────────────────────────────────────────────────── │
│  VHE (KVM-VHE)   │  CPU has ARMv8.1 VHE (ID_AA64MMFR1.VH=1)              │
│                   │  AND booted at EL2                                     │
│                   │  AND HVHE override not disabled                        │
├───────────────────┼──────────────────────────────────────────────────────── │
│  nVHE (KVM-nVHE) │  CPU lacks VHE                                         │
│                   │  OR HVHE override disabled                             │
│                   │  AND booted at EL2                                     │
├───────────────────┼──────────────────────────────────────────────────────── │
│  pKVM             │  Protected KVM: special nVHE mode where KVM at EL2     │
│  (Protected KVM)  │  is isolated from the host kernel (CONFIG_PKVM)        │
│                   │  Host kernel cannot read/write guest memory            │
└───────────────────┴────────────────────────────────────────────────────────┘
```

---

## 2. VHE Mode: Host Kernel Is the Hypervisor

With VHE, the entire Linux kernel runs at EL2. KVM is just another kernel
subsystem, not a separate EL2 component:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        VHE Execution Model                             │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  EL2 (host): ┌─────────────────────────────────────────────┐          │
│              │  Linux kernel (full OS at EL2)              │          │
│              │  KVM subsystem (built into kernel)          │          │
│              │  Page tables: swapper_pg_dir                │          │
│              └─────────────────────────────────────────────┘          │
│                            │  VM entry   ↑  VM exit                   │
│  EL1 (guest):┌─────────────────────────────────────────────┐          │
│              │  Guest OS (Linux, Windows, RTOS...)          │          │
│              │  Guest page tables (stage-1, VTTBR_EL2)     │          │
│              └─────────────────────────────────────────────┘          │
│                            │  User syscall  ↑  return                 │
│  EL0 (guest):┌─────────────────────────────────────────────┐          │
│              │  Guest user processes                       │          │
│              └─────────────────────────────────────────────┘          │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### VM Entry/Exit in VHE Mode

VM entry (guest → host transition):
```
1. Guest EL1 exception → trap to EL2 (TGE bit causes EL0→EL2 directly)
2. KVM saves guest context (x0-x30, system regs) to vcpu struct
3. Restores host context
4. Returns to host kernel at EL2
→ No exception level switch! (already at EL2)
→ Cost: ~500 cycles (register save/restore only)
```

---

## 3. nVHE Mode: Separate EL2 Hypervisor Stub

Without VHE, KVM requires a dedicated EL2 component (`kvm_hyp.S`) that is
separate from the host kernel:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        nVHE Execution Model                            │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  EL2 (HYP): ┌─────────────────────────────────────────────┐           │
│             │  KVM hypervisor stub (kvm/hyp/*.S)          │           │
│             │  Minimal code: vcpu save/restore            │           │
│             │  Separate, small page tables (hyp_pgd)      │           │
│             └─────────────────────────────────────────────┘           │
│                   ↑ hvc          ↓ eret                               │
│  EL1 (host):┌─────────────────────────────────────────────┐           │
│             │  Linux kernel (at EL1)                      │           │
│             │  KVM subsystem (calls EL2 via HVC)          │           │
│             │  Page tables: swapper_pg_dir                │           │
│             └─────────────────────────────────────────────┘           │
│                   ↑ hvc          ↓ eret                               │
│  EL1 (guest):┌────────────────────────────────────────────┐           │
│             │  Guest OS (runs in EL1 with EL2 oversight)  │           │
│             └─────────────────────────────────────────────┘           │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### VM Entry/Exit in nVHE Mode

```
Host (EL1) initiates VM entry:
  1. KVM calls kvm_call_hyp() → HVC → EL2 hypervisor stub
  2. EL1 → EL2 exception level switch (save all EL1 context)
  3. Stub restores guest EL1 context
  4. eret to guest EL1 (EL2 → EL1)

Guest exits (trap to hypervisor):
  5. Guest EL1/EL0 exception → trap to EL2
  6. EL2 stub saves guest EL1 context
  7. Restores host EL1 context
  8. eret to host EL1 (EL2 → EL1)
  9. Host EL1 KVM handles the exit

→ TWO exception level switches per VM entry/exit
→ Cost: ~800 cycles
```

---

## 4. pKVM: Protected KVM — Security-First Design

pKVM (Protected KVM, `CONFIG_PKVM`) is a special nVHE variant designed to
**protect guest memory from the host kernel**. This is the model used by
Android's Confidential VM (CVM) and similar TEE-backed virtualization.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        pKVM Security Model                             │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  EL2 (pKVM): ┌───────────────────────────────────────────────────┐    │
│              │  Protected KVM hypervisor                        │    │
│              │  • Controls guest memory mappings                │    │
│              │  • Host CANNOT access guest pages               │    │
│              │  • Guest pages unmapped from host stage-1       │    │
│              │  • Cryptographic isolation (per-guest key)       │    │
│              └───────────────────────────────────────────────────┘    │
│                   ↑ HVC (restricted)  ↓ controlled eret               │
│  EL1 (host):┌───────────────────────────────────────────────────┐     │
│             │  Android host kernel                              │     │
│             │  • Can create/destroy VMs                        │     │
│             │  • CANNOT read/write guest memory directly       │     │
│             │  • Memory sharing requires explicit grant/reclaim │     │
│             └───────────────────────────────────────────────────┘     │
│                                                                        │
│  EL1 (guest):┌───────────────────────────────────────────────────┐    │
│              │  Protected VM (sensitive workload)               │    │
│              │  • Memory protected from host kernel attack      │    │
│              │  • Can be DRM content rendering, secure payment  │    │
│              └───────────────────────────────────────────────────┘    │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### pKVM and `finalise_el2`

pKVM **requires nVHE** (not VHE), because pKVM's EL2 code needs to be a
**separate, minimal, auditable component** isolated from the host kernel.
If VHE were used, the host kernel (a large, complex codebase) would run at
EL2 with full hypervisor privilege — too large an attack surface for a
security-sensitive hypervisor.

The `arm64_sw_feature_override` HVHE bit can force nVHE even on VHE-capable
hardware, which is used by pKVM builds.

---

## 5. `__boot_cpu_mode` Array: The KVM Eligibility Check

```c
// arch/arm64/include/asm/virt.h

extern u32 __boot_cpu_mode[2];

// Check if ALL CPUs booted at EL2 (required for KVM)
static inline bool is_hyp_mode_available(void)
{
    if (is_pkvm_initialized())
        return true;          // pKVM manages this independently

    return (__boot_cpu_mode[0] == BOOT_CPU_MODE_EL2 &&
            __boot_cpu_mode[1] == BOOT_CPU_MODE_EL2);
}

// Check if CPUs booted in DIFFERENT modes (bootloader bug)
static inline bool is_hyp_mode_mismatched(void)
{
    if (is_pkvm_initialized())
        return false;

    return __boot_cpu_mode[0] != __boot_cpu_mode[1];
}
```

### How KVM Uses These Checks

```c
// arch/arm64/kvm/arm.c
int kvm_arm_init(void)
{
    if (!is_hyp_mode_available()) {
        kvm_info("HYP mode not available\n");
        return -ENODEV;    // KVM not available on this system
    }

    if (is_hyp_mode_mismatched()) {
        kvm_err("KVM cannot support systems where CPUs boot in different modes\n");
        return -ENODEV;
    }
    // ... proceed with KVM initialization
}
```

### Boot Mode Array Fill Protocol

```
Primary CPU (this function, set_cpu_boot_mode_flag in __primary_switched):
  __boot_cpu_mode[0] = BOOT_CPU_MODE_EL1 or EL2

Secondary CPUs (set_cpu_boot_mode_flag in __secondary_switched):
  __boot_cpu_mode[1] = BOOT_CPU_MODE_EL1 or EL2
                      (same write → overwrites previous secondary's write,
                       but they should all write the same value)

kvm_arm_init() (called from start_kernel → do_initcalls):
  checks both [0] and [1] → both must be EL2 for KVM to proceed
```

**NVIDIA Engineering Note**: The two-element array design means that KVM
initialization is gated on both the primary CPU and at least one secondary
CPU reporting EL2. A single bad secondary CPU (with a bootloader bug that
starts it at EL1) will prevent KVM from initializing, even though the
primary works fine. This is intentional — it is safer to refuse KVM
entirely than to attempt EL2 operations on a mixed-mode SMP system.

---

## 6. Performance Impact of VHE/nVHE on Real Workloads

### NVIDIA Jetson/Orin Context (ARM Cortex-A78 / Carmel)

On NVIDIA Tegra platforms, `finalise_el2` typically succeeds on modern SoCs
(Orin: Cortex-A78AE has ARMv8.2 including VHE). The VHE path is taken.

```
Measured VM entry/exit overhead (Cortex-A78AE, Orin):
  VHE mode:   ~450-550 cycles
  nVHE mode:  ~700-900 cycles

For NVIDIA vGPU workloads (frequent VM exits for DMA mapping):
  VM exits/second: ~500,000 (typical GPU command submission workload)
  VHE  overhead: 500K × 500  cycles = 250B cycles/sec ≈  8.3% on 3GHz core
  nVHE overhead: 500K × 800  cycles = 400B cycles/sec ≈ 13.3% on 3GHz core
  VHE advantage: ~5% lower overhead for vGPU workloads
```

### Google Cloud / AMD EPYC Equivalent Context

For ARM64 server-class CPUs (Ampere Altra, Graviton3), VHE is universally
supported and taken. The performance advantage is most visible in:
- High-IOPS storage virtualization (many VM exits per second)
- Network-intensive VMs (frequent vNIC interrupt injections)
- Container workloads with high syscall rates

---

## 7. Design Decision Rationale: Why VHE Was Introduced in ARMv8.1

The original ARMv8.0 model (nVHE only) had a fundamental tension:

**Problem**: The Linux kernel is written for EL1. Running it at EL1 with a
KVM hypervisor at EL2 requires:
1. Every EL1 system register write to be potentially shadowed by EL2
2. Context switches between EL1 (Linux) and EL2 (KVM)
3. Separate page tables for EL2 (security and management overhead)
4. All kernel code remains correct and unmodified for EL1

**VHE Solution**: Run the kernel at EL2 directly. The aliasing mechanism
(`E2H=1`) means the kernel code requires zero changes — `msr sctlr_el1`
just happens to write `sctlr_el2`, which is exactly what we want.

```
Design Priorities (in order):
  1. Zero kernel code changes         ← Achieved via register aliasing
  2. Lower VM exit overhead           ← Achieved via removing EL1↔EL2 switch
  3. Simplified KVM code              ← Achieved (KVM is just a kernel module)
  4. Security (not the main driver)   ← pKVM added later to address this
```

The `finalise_el2` + `enter_vhe` sequence is the ARM architecture's answer
to the "how do you promote a running EL1 OS to EL2 without rebooting?"
problem. The solution — transfer all MM state, patch SPSR, re-enable MMU
from idmap — is elegant but requires deep understanding of ARM's memory
model and exception architecture to implement correctly.
