# `arch_cpu_finalize_init()` — CPU Feature Finalization

## Purpose

Performs final architecture-specific CPU initialization steps that require the full kernel environment (memory allocators, ACPI, etc.) to be available. On x86, this finalizes CPU feature flags and applies any remaining CPU workarounds.

## Source File

`arch/x86/kernel/cpu/common.c`

```c
void __init arch_cpu_finalize_init(void)
{
    identify_boot_cpu();
    
    if (!IS_ENABLED(CONFIG_SMP)) {
        pr_info("CPU: ");
        print_cpu_info(&boot_cpu_data);
    }
    
    x86_init.timers.wallclock_init();
    
    // Apply microcode updates if available:
    microcode_init();
    
    // Enable any deferred CPU capabilities:
    fpu__init_system_xstate();
}
```

## CPU Feature Detection

By this point in boot, the kernel has already run CPUID (in `setup_arch()`). However, some features require:
- Memory allocators (to allocate per-CPU buffers for AVX-512 state)
- ACPI (to read power management capabilities)
- Timers (to calibrate TSC)

### x86 Feature Words

CPU capabilities are stored in `boot_cpu_data.x86_capability[]`:

```c
// Checking a feature:
if (cpu_has(&boot_cpu_data, X86_FEATURE_AVX512F))
    // AVX-512 Foundation available

// Static key (checked at compile time with alternatives):
if (static_cpu_has(X86_FEATURE_IBRS))
    // Spectre v2 mitigation via IBRS
```

### x86 CPU Vulnerabilities

Modern CPUs have microarchitectural vulnerabilities. Feature finalization applies mitigations:

| Vulnerability | Mitigation |
|--------------|------------|
| Spectre v1 | Array index masking, swapgs barriers |
| Spectre v2 | IBRS, IBPB, retpoline, eIBRS |
| Meltdown | KPTI (separate page tables for user/kernel) |
| MDS/TAA | VERW instruction, MD_CLEAR flush |
| SRBDS | SRBDS_CTRL MSR |
| Retbleed | RSB filling, JMPABS |

## FPU/SIMD State

`fpu__init_system_xstate()` finalizes the FPU save/restore mechanism:

```c
// XSAVE areas:
// Bit 0: x87 FPU (legacy)
// Bit 1: SSE (XMM registers)
// Bit 2: AVX (YMM registers)
// Bit 5: AVX-512 opmask
// Bit 6: AVX-512 ZMM_Hi256
// Bit 7: AVX-512 Hi16_ZMM
// Bit 9: PKRU (Memory Protection Keys)
```

The XSAVE area size depends on which features are enabled — this is why FPU finalization requires memory allocation.

## Microcode Updates

On x86, CPU microcode patches are loaded by `microcode_init()`:
- Intel: microcode updates applied via BIOS or OS loader
- AMD: `microcode/AuthenticAMD.bin` applied at early boot
- Late microcode loading (here) handles updates not applied by bootloader

## Cross-references

- [Phase overview](../README.md)
- `setup_arch()`: [../../02_arch_setup/setup_arch/README.md](../../02_arch_setup/setup_arch/README.md) — initial CPU detection
