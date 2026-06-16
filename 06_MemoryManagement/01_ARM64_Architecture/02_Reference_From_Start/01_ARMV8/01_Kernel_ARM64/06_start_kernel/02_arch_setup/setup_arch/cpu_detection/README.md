# `setup_arch()` — CPU Detection & Feature Flags

## Overview

As part of `setup_arch()`, the kernel performs detailed CPU identification and feature detection. This information drives critical decisions throughout the rest of boot: which security mitigations to apply, whether certain instructions are available, and how to configure per-CPU data structures.

## `early_cpu_init()` — Vendor Detection

The first step is identifying the CPU vendor and family using the `CPUID` instruction:

```c
// Pseudo-code of early_cpu_init()
cpuid(0, &eax, &ebx, &ecx, &edx);  // Max CPUID leaf + vendor string
// EBX:EDX:ECX = "GenuineIntel" or "AuthenticAMD" etc.

cpuid(1, &eax, &ebx, &ecx, &edx);  // Family/Model/Stepping
// EAX bits [27:20] = Extended Family, [19:16] = Extended Model, etc.
```

Result stored in `struct cpuinfo_x86 boot_cpu_data`:

```c
struct cpuinfo_x86 {
    __u8 x86_vendor;          // X86_VENDOR_INTEL, X86_VENDOR_AMD, etc.
    __u8 x86;                 // CPU family (e.g., 6 = Pentium Pro+)
    __u8 x86_model;           // CPU model within family
    __u8 x86_stepping;        // Silicon revision
    __u32 x86_capability[NCAPINTS];  // Feature flags
    char x86_vendor_id[16];   // "GenuineIntel" etc.
    char x86_model_id[64];    // "Intel(R) Core(TM) i7-..."
    /* ... many more fields ... */
};
```

## CPU Feature Flags — `x86_capability[]`

CPU features are tracked in a bitmap array of 32-bit words. Each bit corresponds to a specific CPU feature:

```c
// Feature words (NCAPINTS = 21 on recent kernels):
// Word 0:  CPUID(1).EDX  — classic features (FPU, MMX, SSE, SSE2, ...)
// Word 1:  CPUID(1).ECX  — extended features (SSE3, PCLMULQDQ, AVX, ...)
// Word 4:  CPUID(0x80000001).EDX — AMD extended (SYSCALL, NX, LM, ...)
// Word 9:  Virtual features (software-defined, e.g., 32-bit, NONSTOP_TSC)
// Word 16: CPUID(7).EBX  — structured extended features (AVX2, BMI2, ...)
// ... etc
```

Access via:
```c
if (cpu_has(&boot_cpu_data, X86_FEATURE_AVX2)) { ... }
if (boot_cpu_has(X86_FEATURE_NX)) { ... }      // Current CPU
if (this_cpu_has(X86_FEATURE_CLFLUSH)) { ... } // Per-CPU check
```

## Key Features Detected

| Feature | Bit | Significance |
|---------|-----|--------------|
| `X86_FEATURE_NX` | EFER.NXE | No-Execute page protection |
| `X86_FEATURE_SMEP` | CR4.SMEP | Supervisor Mode Execution Prevention |
| `X86_FEATURE_SMAP` | CR4.SMAP | Supervisor Mode Access Prevention |
| `X86_FEATURE_PTI` | synthetic | Page Table Isolation (Meltdown mitigation) |
| `X86_FEATURE_IBRS` | MSR | Indirect Branch Restricted Speculation |
| `X86_FEATURE_RDRAND` | ECX.30 | Hardware random number generator |
| `X86_FEATURE_TSC_RELIABLE` | synthetic | TSC suitable as clocksource |
| `X86_FEATURE_CONSTANT_TSC` | synthetic | TSC does not vary with CPU speed |

## Microcode Loading

`setup_arch()` calls `load_ucode_bsp()` early to load CPU microcode from the initrd. Microcode updates:
- Fix CPU bugs (errata)
- Add new instructions
- Enable security mitigations (Spectre, MDS)
- Must be loaded before the CPU feature flags are finalized

## Errata Workarounds

Based on vendor + family + model + stepping, the kernel applies CPU-specific workarounds:
```c
void apply_microcode_early(struct ucode_cpu_info *uci, bool use_warmreset)
```

Examples:
- Disabling a broken performance counter
- Working around a CPUID instruction bug
- Fixing incorrect TSC behavior

## Result

After CPU detection:
- `boot_cpu_data` is fully populated
- `cpu_possible_mask` has been sized correctly for the topology
- Security mitigation decisions have been made (PTI, IBRS, STIBP)
- `X86_FEATURE_*` bits are correct for the boot CPU

## Cross-references

- [setup_arch overview](../README.md)
- `arch_cpu_finalize_init()` — final CPU setup: [../../../14_acpi/arch_cpu_finalize_init/README.md](../../../14_acpi/arch_cpu_finalize_init/README.md)
