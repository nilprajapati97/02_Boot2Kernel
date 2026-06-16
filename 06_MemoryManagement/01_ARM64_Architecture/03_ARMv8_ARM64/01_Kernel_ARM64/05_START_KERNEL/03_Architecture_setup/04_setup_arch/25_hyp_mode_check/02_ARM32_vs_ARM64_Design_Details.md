# hyp_mode_check — ARM32 vs ARM64 Design Details

## 1. ARM32: hyp_mode_check() is Relevant

On ARM32, HYP mode (EL2) is optional — not all bootloaders start the kernel in HYP mode. The check is meaningful.

```c
/* ARM32: hyp_mode_check() prints diagnostic message */
if (is_hyp_mode_available())
    pr_info("CPU: All CPU(s) started in HYP mode.\n");
else if (is_hyp_mode_mismatched())
    pr_warn("CPU: WARNING: inconsistent modes ...\n");
else
    pr_info("CPU: All CPU(s) started in SVC mode.\n");
```

**Effect on KVM:**
- HYP mode available → `kvm_arch_init()` uses hardware virtualization
- SVC mode only → KVM unavailable (returns `-ENODEV`)

---

## 2. ARM64: EL2 Is Always Available (No Check Needed)

On ARM64, the existence of EL2 is architectural — all ARMv8-A implementations have EL2. ARM64 `setup_arch()` does NOT call `hyp_mode_check()`. Instead, the boot code detects the current EL at startup:

```c
/* arch/arm64/kernel/head.S */
SYM_CODE_START(primary_entry)
    ...
    bl      init_kernel_el    /* Detect EL2/EL1, configure appropriately */
```

```c
/* arch/arm64/kernel/hyp-stub.S */
/* If booting at EL2: install HYP stub so kernel can switch back to EL2 later */
/* If booting at EL1: record that EL2 was not available at boot */
```

The ARM64 kernel can boot at either EL1 or EL2. If it boots at EL2 with VHE (`HCR_EL2.E2H=1`), it continues running at EL2 the whole time (Linux Host Extensions). If it boots at EL2 without VHE, it drops to EL1 but installs a small HYP stub to re-enter EL2 when KVM initializes.

---

## 3. EL2 Boot Scenarios: ARM32 vs ARM64

### ARM32 Boot Scenarios

```
Scenario A: U-Boot → kernel in HYP mode
  CPSR.mode = 0x1A (HYP)
  __boot_cpu_mode[0] = HYP
  KVM: available

Scenario B: U-Boot → kernel in SVC mode
  CPSR.mode = 0x13 (SVC)
  __boot_cpu_mode[0] = SVC
  KVM: unavailable

Scenario C: ATF at EL3 → drops to HYP → kernel
  CPSR.mode = 0x1A (HYP)
  KVM: available
```

### ARM64 Boot Scenarios

```
Scenario A: ATF drops to EL1 (old/minimal firmware)
  CurrentEL = EL1
  HYP stub: NOT installed
  KVM: unavailable (no EL2 entry point)
  Note: Rare, non-compliant with ARM BSA

Scenario B: ATF drops to EL2 (standard)
  CurrentEL = EL2
  Kernel: installs HYP stub, drops to EL1
  KVM: installs handlers in EL2 via HYP stub
  KVM: available

Scenario C: ATF drops to EL2, VHE enabled
  CurrentEL = EL2, HCR_EL2.E2H = 1
  Kernel: runs at EL2 directly (VHE mode)
  KVM: guests run at EL1 (kernel at EL2, guest at EL1, user at EL0)
```

---

## 4. VHE (Virtualization Host Extensions): ARM64 Specific

VHE (ARMv8.1 feature) allows the host OS kernel to run at EL2:

```
Without VHE:
  EL0: Guest user    EL0: Host user
  EL1: Guest kernel  EL1: Host kernel (Linux)
  EL2: KVM hypervisor
  (KVM must trap every EL1/EL0 register access)

With VHE:
  EL0: Guest user    EL0: Host user
  EL1: Guest kernel
  EL2: Host kernel (Linux) ← runs here directly!
  (No EL2 trap overhead for host operations)
```

VHE dramatically reduces VM exit overhead. ARM32 has no VHE equivalent.

---

## 5. __boot_cpu_mode Storage

### ARM32

```c
/* arch/arm/include/asm/virt.h */
extern unsigned int __boot_cpu_mode[2];
/* [0] = boot CPU mode, set in head.S */
/* [1] = secondary CPUs' mode, set in secondary_startup */
```

### ARM64

```c
/* arch/arm64/include/asm/virt.h */
extern u64 __boot_cpu_mode[2];
/* [0] = boot CPU EL level (EL1 or EL2), set in el2_setup */
/* [1] = secondary CPUs' EL level, set in el2_setup for secondaries */
```

---

## 6. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| HYP/EL2 presence | Optional (implementation-defined) | Mandatory (ARMv8-A architectural) |
| hyp_mode_check() | Yes, in setup_arch() | Not needed |
| Boot CPU mode | SVC or HYP (0x13 or 0x1A) | EL1 or EL2 |
| VHE support | No | Yes (ARMv8.1+) |
| KVM if no HYP | Unavailable | Unavailable (EL1-only boot) |
| KVM if HYP/EL2 | Stage-2 virtualization | Stage-2 or VHE |
| Mismatch warning | Yes | Not needed (hardware guarantees EL2) |
| __boot_cpu_mode type | u32 (CPU mode bits) | u64 (CurrentEL value) |
