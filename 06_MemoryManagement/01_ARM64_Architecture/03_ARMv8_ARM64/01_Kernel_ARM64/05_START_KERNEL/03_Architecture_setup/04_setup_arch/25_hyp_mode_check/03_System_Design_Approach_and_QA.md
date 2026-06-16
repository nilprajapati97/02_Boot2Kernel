# hyp_mode_check — System Design Approach and Q&A

## 1. Why Boot in HYP Mode?

The ARM architecture requires entering EL2 **from a higher EL** (EL3 or EL2 itself). Once in EL1 (SVC mode), there's no way to self-elevate to EL2. This is a fundamental security boundary: EL2 can inspect and control EL1, but EL1 cannot modify EL2 settings.

Consequence: **The bootloader must place the kernel in HYP mode if KVM is desired.** The kernel cannot "upgrade" itself to HYP mode after boot.

```
Boot flow with KVM support:
  Power on → Boot ROM → U-Boot/firmware → HYP mode → Kernel → KVM ready

Boot flow without KVM support:
  Power on → Boot ROM → U-Boot → SVC mode → Kernel → KVM unavailable
```

This is why `hyp_mode_check()` just reports the mode — it can't change it. It's informational only.

---

## 2. The HYP Stub Mechanism

On systems that boot at HYP mode, the kernel installs a small "HYP stub" before dropping to SVC/EL1:

```c
/* arch/arm/kernel/hyp-stub.S */
ENTRY(__hyp_stub_install)
    /* Save a small vector table at HYP mode vectors */
    /* This allows re-entering HYP mode later via HVC */
    store_primary_cpu_mode r4, r5, r6
    ...
    /* Switch from HYP to SVC */
    mrs r4, cpsr
    bic r4, r4, #MODE_MASK
    orr r4, r4, #SVC_MODE
    msr spsr_cxsf, r4
    adr lr, __hyp_stub_install_end
    msr elr_hyp, lr
    eret   /* Enter SVC mode */
ENDPROC(__hyp_stub_install)
```

The stub acts as a "door" — after the kernel drops to SVC, KVM can use `HVC` instructions to pass through the stub back into HYP mode where KVM's hypervisor code runs.

---

## 3. Dependency Graph

```
[Bootloader starts kernel in HYP mode or SVC mode]
        │
[head.S: save __boot_cpu_mode[0] = current mode]
        │
[setup_arch()]
  ├── if is_smp():
  │     smp_set_ops / smp_init_cpus / smp_build_mpidr_hash
  │     (secondary CPUs set __boot_cpu_mode[1] in secondary_startup)
  └── if !is_smp():
        hyp_mode_check() → reads __boot_cpu_mode[0,1]
                         → prints diagnostic
        │
[KVM initialization — later, after start_kernel]
  └── kvm_arch_init():
        if (!is_hyp_mode_available()) return -ENODEV;
        /* Install KVM vectors in HYP mode */
        init_hyp_mode()
          → kvm_call_hyp(__kvm_hyp_init, ...)
```

---

## 4. System Design Q&A

**Q: If hyp_mode_check() is just diagnostic (print only), why is it in setup_arch() at all?**
> Two reasons: (1) Timing — it must run after secondary CPUs have set `__boot_cpu_mode[1]` (or on UP where `[1]` is checked by `is_hyp_mode_mismatched()`). Placing it at the end of `setup_arch()` ensures secondaries have already run `sync_boot_mode()`. (2) Visibility — the message "All CPU(s) started in HYP mode" or the mismatch warning appears early in `dmesg` before driver initialization, making it easy to spot. If a user reports KVM isn't working, the first thing to check is this message in `dmesg`. Placing it near driver init would make it harder to find. The function is essentially a documentation-at-runtime tool.

**Q: What is the security concern with HYP mode? Why not run the whole kernel in HYP mode?**
> Running the Linux kernel in HYP mode (EL2) on ARM32 would give the kernel access to stage-2 page tables, which control what physical addresses EL1 can access. If the kernel ran at EL2, it would have access to modify the same memory translation that a hypervisor uses to isolate VMs — there would be no isolation boundary. ARM32 therefore uses a split model: kernel in SVC/EL1, KVM hypervisor routines in HYP/EL2. On ARM64, VHE solves this by redesigning EL2 so the host OS can run there without the security implications (guest OS runs at EL1 under EL2 control), providing isolation while avoiding the context-switch overhead of the EL1/EL2 split model.

**Q: What does sync_boot_mode() do on SMP and why is it not needed on UP?**
> `sync_boot_mode()` is called from `secondary_start_kernel()` (the C entry point of secondary CPUs). Each secondary CPU checks its own HYP mode availability and updates `__boot_cpu_mode[1]` to reflect whether all secondary CPUs consistently have HYP mode. If any secondary finds a mismatch (was not started in HYP mode when boot CPU was), `__boot_cpu_mode[1]` is set to indicate mismatch. On UP builds, there are no secondary CPUs — `secondary_start_kernel()` never runs — so `__boot_cpu_mode[1]` remains unset/zero. The `hyp_mode_check()` path for UP only checks `[0]` effectively, and the mismatch path is unreachable in practice (comparing [0] vs [1] where [1]=0 would look like mismatch, but `is_hyp_mode_mismatched()` has logic to handle the UP case).
