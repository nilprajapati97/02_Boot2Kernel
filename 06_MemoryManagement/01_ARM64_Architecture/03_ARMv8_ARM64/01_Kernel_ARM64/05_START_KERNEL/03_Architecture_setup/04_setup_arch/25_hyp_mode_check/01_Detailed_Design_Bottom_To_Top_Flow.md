# hyp_mode_check — Detailed Design Bottom-To-Top Flow

## 1. The Code

```c
/* arch/arm/kernel/setup.c */
if (!is_smp())
    hyp_mode_check();
```

This is called only on **UP (Uniprocessor)** builds — when `is_smp()` is false. On SMP builds, this is skipped because the SMP block already handles the CPU mode configuration.

---

## 2. Why Hyp Mode Matters for KVM

ARM32 KVM (Kernel-based Virtual Machine) requires the CPU to be running in **HYP mode (EL2)** or at minimum be able to switch to HYP mode:

```
Exception Levels:
  EL0: User space (NS.EL0)
  EL1: Linux kernel (NS.EL1)
  EL2: Hypervisor / KVM (NS.EL2) ← HYP mode in AArch32
  EL3: Secure Monitor / ATF (S.EL3)

For KVM to work:
  Boot CPU must start in HYP mode or have HYP mode available
  All secondary CPUs must also have consistent HYP mode state
```

If CPUs in a multi-CPU system have inconsistent HYP mode state (some booted in HYP, some didn't), KVM cannot work reliably.

---

## 3. hyp_mode_check() Source

**File:** `arch/arm/kernel/setup.c`

```c
static void __init hyp_mode_check(void)
{
    if (is_hyp_mode_available()) {
        pr_info("CPU: All CPU(s) started in HYP mode.\n");
        pr_info("CPU: Virtualization extensions available.\n");
    } else if (is_hyp_mode_mismatched()) {
        pr_warn("CPU: WARNING: CPU(s) started in wrong/inconsistent modes (primary CPU mode 0x%x)\n",
                __boot_cpu_mode & MODE_MASK);
        pr_warn("CPU: This may indicate a broken bootloader or firmware.\n");
    } else {
        pr_info("CPU: All CPU(s) started in SVC mode.\n");
    }
}
```

---

## 4. is_hyp_mode_available() and __boot_cpu_mode

```c
/* arch/arm/include/asm/virt.h */
extern unsigned int __boot_cpu_mode[2];

/* __boot_cpu_mode[0]: boot CPU's mode
 * __boot_cpu_mode[1]: secondary CPUs' consistent mode (set during SMP boot) */

static inline bool is_hyp_mode_available(void)
{
    return (__boot_cpu_mode[0] == HYP_MODE &&
            __boot_cpu_mode[1] == HYP_MODE);
}

static inline bool is_hyp_mode_mismatched(void)
{
    return __boot_cpu_mode[0] != __boot_cpu_mode[1];
}
```

`__boot_cpu_mode` is set during early boot:

```c
/* arch/arm/kernel/head.S */
__HEAD
ENTRY(stext)
    ...
    /* Save current CPU mode */
    mrs r5, cpsr
    and r5, r5, #MODE_MASK
    str r5, [r10, #PROCINFO_CPU_VAL]
    /* Store in __boot_cpu_mode[0] */
    ...
```

---

## 5. What is HYP Mode (EL2) vs SVC Mode (EL1)?

```
ARM32 CPU modes:
  USR (0x10): User mode — applications
  FIQ (0x11): Fast interrupt
  IRQ (0x12): Normal interrupt
  SVC (0x13): Supervisor — Linux kernel runs here normally
  ABT (0x17): Abort
  UND (0x1B): Undefined instruction
  SYS (0x1F): System (privileged user mode)
  HYP (0x1A): Hypervisor — EL2 equivalent in AArch32
  MON (0x16): Monitor — EL3 equivalent (secure world)
```

If the bootloader (U-Boot) starts the kernel in HYP mode (`CPSR = 0x1A`), KVM can use hardware virtualization. If the kernel boots in SVC mode (`CPSR = 0x13`), KVM is unavailable (or runs in software emulation mode on some configurations).

---

## 6. Boot CPU Mode vs Secondary CPU Mode Consistency

On SMP systems, all CPUs must be in the same mode for KVM to work:

```
Scenario 1: All CPUs in HYP mode
  __boot_cpu_mode[0] = HYP (0x1A)  ← boot CPU (set in head.S)
  __boot_cpu_mode[1] = HYP (0x1A)  ← secondaries (set in secondary_startup)
  is_hyp_mode_available() = true
  → KVM works, message: "All CPU(s) started in HYP mode"

Scenario 2: All CPUs in SVC mode
  __boot_cpu_mode[0] = SVC (0x13)
  __boot_cpu_mode[1] = SVC (0x13)
  is_hyp_mode_available() = false
  is_hyp_mode_mismatched() = false
  → KVM unavailable, message: "All CPU(s) started in SVC mode"

Scenario 3: Mismatch (broken bootloader)
  __boot_cpu_mode[0] = HYP (0x1A)  ← boot CPU
  __boot_cpu_mode[1] = SVC (0x13)  ← secondaries (bootloader bug)
  is_hyp_mode_mismatched() = true
  → WARNING: "CPU(s) started in wrong/inconsistent modes"
```

---

## 7. Interview Q&A

**Q1: Why is hyp_mode_check() only called when !is_smp()?**
> On SMP builds, `sync_boot_mode()` is called during secondary CPU bringup (`secondary_start_kernel()`) to synchronize `__boot_cpu_mode[1]` with all secondary CPUs. The SMP path already handles the mode consistency check. On UP builds (single CPU, no secondary CPUs), `sync_boot_mode()` never runs, so `__boot_cpu_mode[1]` may be in its initial state. `hyp_mode_check()` on UP builds essentially just checks `__boot_cpu_mode[0]` (the boot CPU's mode). The `if (!is_smp())` guard prevents the warning about mismatched modes from firing incorrectly on SMP builds before secondaries have set their mode.

**Q2: Can the kernel switch to HYP mode if the bootloader started it in SVC mode?**
> No. Once the CPU is in SVC mode, it cannot self-promote to HYP mode — HYP mode can only be entered from a higher privilege level (EL3/Monitor mode). The bootloader or firmware at EL3 (ATF) would need to set up the CPU in HYP mode before jumping to the kernel. Some bootloaders (U-Boot with `CONFIG_ARMV7_VIRT`) support jumping to the kernel in HYP mode. If the bootloader doesn't support it, users can use the `hvc` stub approach: the bootloader jumps to a small stub that drops to HYP mode before entering the kernel. Modern systems use PSCI which handles mode setup at EL3/ATF level.

**Q3: How does KVM on ARM32 use HYP mode differently from KVM on ARM64?**
> ARM32 KVM uses a "split mode" approach: the kernel runs in SVC mode (EL1), and when a VM is active, KVM temporarily switches to HYP mode (EL2) to handle VM exits. This requires HYP mode to be available at boot. ARM64 KVM is simpler: the kernel normally runs in EL1, and KVM transitions to EL2 via `hvc` instructions. On ARM64, EL2 is always available if `HCR_EL2.E2H = 0` (VHE disabled) or the kernel itself runs at EL2 with VHE (Virtualization Host Extension, `HCR_EL2.E2H = 1`). ARM64 VHE is a major improvement: the kernel runs natively at EL2, eliminating the mode switch overhead for VM exits.
