# ARMv8 / ARM64 full boot big-picture diagram

Your ARM64 `setup_arch()` note already captured the key stages: bootloader provides DTB, kernel sets early mappings, parses FDT, initializes memblock, paging, PSCI, CPU topology, and MPIDR mapping .

```mermaid
flowchart TD
    A[Bootloader / Firmware] --> B[Load Image into RAM]
    A --> C[Place DTB in RAM]
    A --> D[x0 = DTB physical address]

    B --> E[Kernel entry: arch/arm64/kernel/head.S]
    D --> E

    E --> F[Save x0 as __fdt_pointer]
    E --> G[Create early identity mapping]
    E --> H[Create early kernel mapping]
    G --> I[Enable MMU + caches]
    H --> I

    I --> J[start_kernel]
    J --> K[setup_arch]

    K --> L[setup_initial_init_mm]
    L --> M[early_fixmap_init / early_ioremap_init]

    M --> N[setup_machine_fdt __fdt_pointer]
    C --> N
    N --> O[Temporarily map DTB]
    O --> P[early_init_dt_scan]

    P --> Q[Read memory nodes]
    P --> R[Read chosen node: bootargs/initrd]
    P --> S[Read CPU nodes]
    P --> T[Read reserved-memory]

    Q --> U[memblock.memory]
    T --> V[memblock.reserved]
    R --> W[boot_command_line / initrd info]

    U --> X[arm64_memblock_init]
    V --> X
    X --> Y[paging_init]
    Y --> Z[Build final kernel page tables]
    Z --> AA[Kernel linear map ready]

    S --> AB[smp_init_cpus]
    AB --> AC[cpu_logical_map]
    AC --> AD[MPIDR_EL1 mapping]
    AD --> AE[smp_build_mpidr_hash]

    K --> AF[CPU feature discovery]
    AF --> AG[Read MIDR_EL1 / ID_AA64 registers]
    AG --> AH[Detect ARMv8 features + errata]
    AH --> AI[Expose HWCAP to user space later]

    K --> AJ[psci_dt_init / psci_acpi_init]
    AJ --> AK[Firmware interface for CPU on/off/reset]

    AA --> AL[bootmem_init]
    AE --> AL
    AK --> AL
    AL --> AM[Generic kernel init continues]
    AM --> AN[Scheduler, IRQs, drivers, init process]
```

## Interview one-liner

```text
ARM64 boot converts bootloader-provided raw facts — kernel image, DTB pointer in x0, RAM layout, CPU nodes, and firmware interface — into kernel-ready state: virtual memory, memblock, page tables, CPU topology, PSCI operations, and user-visible CPU capabilities.
```

## Best mental model

```text
Bootloader gives raw physical world
        ↓
head.S creates minimal executable virtual world
        ↓
setup_arch parses DTB and discovers memory/CPU topology
        ↓
memblock + paging create real kernel memory world
        ↓
PSCI + CPU maps prepare SMP world
        ↓
generic kernel continues
```
