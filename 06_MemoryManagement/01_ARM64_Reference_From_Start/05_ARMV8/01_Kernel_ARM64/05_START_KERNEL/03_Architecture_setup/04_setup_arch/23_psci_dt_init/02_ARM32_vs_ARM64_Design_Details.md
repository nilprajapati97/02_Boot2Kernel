# psci_dt_init() — ARM32 vs ARM64 Design Details

## 1. Same Function, Different Ecosystem

`psci_dt_init()` is the same function for both architectures (`drivers/firmware/psci/psci.c`). The differences are in:
- PSCI function ID widths (32-bit vs 64-bit)
- How common PSCI is (optional on ARM32, mandatory on ARM64)
- SMC calling convention differences
- MPIDR width passed to CPU_ON

---

## 2. SMCCC: 32-bit vs 64-bit Calling Convention

**ARM SMC Calling Convention (SMCCC)** defines how function IDs and arguments are encoded in SMC/HVC calls:

### ARM32 (SMCCC 32-bit)

```c
/* Function IDs start with 0x84 = 32-bit standard PSCI */
#define PSCI_0_2_FN_CPU_ON          0x84000003U  /* 32-bit */

/* Arguments: r0=function_id, r1=target_cpu, r2=entry_point, r3=context_id */
static void __invoke_psci_fn_smc(u32 fn, u32 arg0, u32 arg1, u32 arg2)
{
    asm volatile(
        "mov r0, %0\n"
        "mov r1, %1\n"
        "mov r2, %2\n"
        "mov r3, %3\n"
        "smc #0\n"
        : : "r"(fn), "r"(arg0), "r"(arg1), "r"(arg2)
        : "r0", "r1", "r2", "r3"
    );
}
```

### ARM64 (SMCCC 64-bit)

```c
/* Function IDs start with 0xC4 = 64-bit standard PSCI */
#define PSCI_0_2_FN64_CPU_ON        0xC4000003UL  /* 64-bit */

/* Arguments: x0=function_id, x1=target_cpu, x2=entry_point, x3=context_id */
static unsigned long __invoke_psci_fn_smc(unsigned long fn,
    unsigned long arg0, unsigned long arg1, unsigned long arg2)
{
    struct arm_smccc_res res;
    arm_smccc_smc(fn, arg0, arg1, arg2, 0, 0, 0, 0, &res);
    return res.a0;
}
```

ARM64 passes 64-bit MPIDRs in x1 (target CPU), enabling full 40-bit Aff3 support.

---

## 3. PSCI on ARM32: Optional (Many Boards Use Custom SMP)

Many ARM32 boards (especially pre-2014) don't support PSCI. They use board-specific SMP operations:

```c
/* Example: Cortex-A9 "pen release" mechanism */
struct smp_operations cortex_a9_smp_ops __initdata = {
    .smp_init_cpus  = platform_smp_init_cpus,
    .smp_boot_secondary = platform_boot_secondary,
    /* platform_boot_secondary writes entry point to shared SRAM scratch reg,
       secondary CPU polls that register and jumps when non-zero */
};
```

If the DT has `enable-method = "spin-table"` instead of "psci", the kernel uses `spin_table_smp_ops` instead.

### ARM32 `enable-method` Options

```
enable-method = "psci"         → use PSCI CPU_ON
enable-method = "spin-table"   → bootloader-specific spin table
enable-method = "release-addr" → write entry addr to a specific physical addr
```

### ARM64 `enable-method` Options

```
enable-method = "psci"         → mandatory on all production ARM64 systems
enable-method = "spin-table"   → used in some FPGA/emulator environments
```

On ARM64, PSCI is effectively mandatory for production hardware. ARM64 silicon that doesn't support PSCI at all is extremely rare.

---

## 4. PSCI CPU_ON: MPIDR Argument Width

### ARM32

```c
psci_ops.cpu_on(cpu_logical_map(cpu), __pa(secondary_entry));
/* cpu_logical_map is u32: passes 24-bit MPIDR */
/* entry_point is u32 physical address (RAM < 4GB) */
```

### ARM64

```c
psci_ops.cpu_on(cpu_logical_map(cpu), __pa_symbol(secondary_entry));
/* cpu_logical_map is u64: passes 40-bit MPIDR (Aff3 support) */
/* entry_point is u64 physical address (supports high-memory) */
```

On multi-socket ARM64 servers with CPUs at different NUMA nodes, the Aff3 field encodes the socket number. Without 64-bit MPIDR support, PSCI can't address CPUs on socket 1+.

---

## 5. Hypervisor Context: SMC vs HVC

```
method = "smc":
  Kernel (EL1) → SMC #0 → ATF (EL3)
  Used: bare-metal, real hardware

method = "hvc":
  Guest VM (EL1) → HVC #0 → Hypervisor (EL2)
  Used: KVM guests, Xen DomU, cloud VMs

ARM32 KVM:
  KVM on ARM32 supports both SMC-based PSCI (pass-through) and HVC PSCI

ARM64 KVM:
  KVM implements a full PSCI interface for guest VMs
  Guest DT has method = "hvc"
  KVM intercepts HVC, emulates CPU_ON by starting vCPU
```

---

## 6. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| PSCI prevalence | Optional (many boards without) | Effectively mandatory |
| Function IDs | 0x8400xxxx (32-bit SMCCC) | 0xC400xxxx (64-bit SMCCC) |
| MPIDR in CPU_ON | u32 (24-bit) | u64 (40-bit) |
| Entry point width | u32 (32-bit physical) | u64 (64-bit physical) |
| method options | smc, hvc | smc, hvc |
| PSCI fallback | spin-table, pen-release, custom | spin-table (FPGA/emulator only) |
| PSCI 1.0 features | Rarely used | Common (SYSTEM_SUSPEND for S3) |
| QEMU support | Yes (PSCI emulated) | Yes (PSCI emulated) |
