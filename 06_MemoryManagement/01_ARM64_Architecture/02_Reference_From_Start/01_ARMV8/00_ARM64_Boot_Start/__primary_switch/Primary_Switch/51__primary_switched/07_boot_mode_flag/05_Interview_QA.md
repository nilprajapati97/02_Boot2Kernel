# Boot Mode Flag — Interview Q&A

---

## Q1: What does `set_cpu_boot_mode_flag` record and why?

**A:** It records whether the primary CPU entered the kernel from EL2 (hypervisor level)
or EL1 (OS level), stored in `__boot_cpu_mode[0]`. This information determines:
1. Whether KVM (Linux's hypervisor) is available — requires EL2 boot
2. Whether VHE mode (ARMv8.1 virtualization host extension) can be used
3. Whether `finalise_el2` should configure EL2 or skip it

Without this information, the kernel cannot make correct decisions about
virtualization capabilities during `kvm_arch_init()`.

---

## Q2: On a typical Qualcomm Snapdragon SoC, at what EL does Linux boot?

**A:** EL2. Qualcomm Snapdragon uses ARM TrustZone extensively. The boot flow:
1. Secure ROM → EL3 (Secure Monitor)
2. TrustZone OS (QSEE/TEE) initialized at EL3/Secure-EL1
3. Bootloader (XBL/SBL) → drops to EL2 (non-secure)
4. Linux kernel enters at EL2
5. Linux `init_kernel_el` configures VHE or nVHE, drops to EL1

`__boot_cpu_mode = BOOT_CPU_MODE_EL2` → KVM available on Snapdragon.
Android uses KVM for virtual machine support (Android Virtualization Framework).

---

## Q3: What is VHE and why does it matter for `__boot_cpu_mode`?

**A:** VHE = Virtualization Host Extensions (ARMv8.1-A). When active (`HCR_EL2.E2H=1`),
the Linux kernel runs natively at EL2 instead of EL1. This eliminates EL1↔EL2
context switching overhead during VM exits, reducing VM exit latency from ~1000+ cycles
to ~100-200 cycles (~10× improvement).

`__boot_cpu_mode = BOOT_CPU_MODE_EL2` is a PREREQUISITE for VHE — you can only
use VHE if the kernel booted at EL2. The `finalise_el2` function (called after
`set_cpu_boot_mode_flag`) checks `__boot_cpu_mode` and `HCR_EL2.E2H` to decide
VHE vs nVHE configuration.

---

## Q4: Can `__boot_cpu_mode` be wrong? What are the consequences?

**A:** If `__boot_cpu_mode` is wrong (e.g., says EL1 but actually booted at EL2):
- `is_hyp_mode_available()` returns false
- KVM module fails to load with "HYP mode not available"
- Virtualization unavailable on a platform that supports it

If wrong in the other direction (says EL2 but actually EL1):
- `finalise_el2` would try to write EL2 system registers
- `msr hcr_el2, x0` would trap to EL3 (firmware) or cause UNDEFINED exception
- Immediate kernel crash

Since `x20` is set by `init_kernel_el` which reads `CurrentEL` hardware register,
it can only be wrong if the `init_kernel_el` code has a bug — not a firmware issue.
The hardware register `CurrentEL` is always correct.

---

## Q5: What is `BOOT_CPU_MODE_EL1 = 0x0e11`? Why that value?

**A:** `0x0e11` is a mnemonic: `0x0E11` where E=14 (hex), 1=1 → "EL1".
Similarly, `0x0E12` = "EL2". These values:
1. Are easy to recognize in hex dumps or memory forensics
2. Are non-zero (so a zeroed memory region doesn't look like valid mode)
3. Are NOT valid ARM64 instruction encodings (reduces false positives)
4. Have distinct values that can be distinguished even in partial memory dumps

It's a Linux convention to use "magic-like" values for mode flags that appear
in memory-dumped kernel data structures.

---

## Q6: How does a hypervisor (KVM) handle the transition when Linux boots at EL2?

**A:** When Linux boots at EL2 with VHE (`HCR_EL2.E2H=1, TGE=1`):
1. Linux kernel runs natively at EL2 (the CPU sees it as EL2, but EL2→EL1 aliasing
   makes EL1 registers alias to EL2 equivalents)
2. User processes run at EL0 (using EL2-rooted page tables when E2H=1, TGE=1)
3. When running a VM: TGE is cleared, VM runs at EL1 (what VM thinks is its EL1)
4. VM exits trap to EL2 (actual EL2 = host Linux kernel)

The `__boot_cpu_mode` record allows `finalise_el2` to properly configure this
VHE host mode. Without knowing we booted at EL2, `finalise_el2` would not
configure EL2 registers, leaving the hypervisor in an incomplete state.

---

## Q7: What happens to `__boot_cpu_mode` when kexec reboots into a new kernel?

**A:** During kexec, the new kernel starts fresh at primary_entry, going through
the full boot sequence including `init_kernel_el` and `set_cpu_boot_mode_flag`.
The new kernel computes `__boot_cpu_mode` fresh from hardware state.

The kexec transition tries to leave the CPU at EL2 (if it was booted at EL2) so
the new kernel can detect EL2 correctly. If kexec inadvertently drops to EL1 before
the new kernel starts, the new kernel would see `BOOT_CPU_MODE_EL1` even though
EL2 is theoretically available — KVM would be disabled in the new kernel.

This is a known subtle kexec+VHE issue that kernel developers have addressed in
recent kernels with explicit EL restoration during kexec.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, general-purpose registers x0-x30 are each 64-bit. Registers x19-x28 are callee-saved per AAPCS64: a function that modifies them must save them on entry and restore them before returning. The boot registers x20 (CPU boot mode) and x21 (FDT pointer) are chosen from the callee-saved range so that they survive through BL __pi_early_map_kernel and BL __enable_mmu without needing to be pushed/popped on the stack. The hardware provides 31 general-purpose registers (x0-x30) plus XZR (always-zero) and SP (stack pointer).

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot register convention in __primary_switch:
- x20: CPU boot mode flags (BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2, and E2H flag).
- x21: FDT physical address (passed from bootloader in x1, saved early in head.S).
- x22: kernel image physical address (phys_offset).
- x23: init_task VA (for SP_EL0 setup).
- x24: TTBR1_EL1 value (kernel page table root PA).
x20 survives all C calls via the callee-save guarantee; it is read in __primary_switched to propagate the boot mode to __boot_cpu_mode (per-CPU variable).

### Memory Perspective (ARMv8 Memory Model)
The callee-saved registers (x19-x28) act as a free "register file" for passing state across function calls without touching memory. This is particularly important in the pre-MMU phase where there is no reliable stack VA. x21 (FDT PA) is kept in a register rather than memory because the stack is at a physical address that may not be mapped after the MMU is enabled. The register file is part of the CPU's architectural state -- it is not cache-coherent (it is in the register file, not RAM) -- so register passing has zero memory latency.