# `setup_arch()` — Architecture-Specific Hardware Setup

## Overview

| Attribute    | Value                                                 |
|-------------|--------------------------------------------------------|
| **Function** | `setup_arch(char **cmdline_p)`                        |
| **Source**   | `arch/x86/kernel/setup.c` (x86), `arch/arm64/kernel/setup.c` (ARM64) |
| **Purpose**  | Detect and initialize all hardware: memory map, CPU features, NUMA topology, page tables |
| **Complexity** | ~1500 lines on x86, calls 50+ sub-functions            |

---

## Why It Exists

This is the **most complex single function call** in `start_kernel()`. The hardware-independent kernel cannot know:
- How much RAM exists or where it is in physical address space
- What NUMA node topology looks like
- What ISA/PCI buses are present
- Where kernel command line parameters came from

`setup_arch()` answers all of these by reading hardware tables (E820, ACPI, Device Tree) and setting up the kernel's view of the hardware.

---

## x86 `setup_arch()` — Deep Walkthrough

### Phase A: Early Memory Detection

```c
// arch/x86/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    // 1. Copy boot parameters from real-mode setup area
    memcpy(&boot_params, _boot_params, sizeof(boot_params));
    
    // 2. Set up E820 memory map from BIOS
    e820__memory_setup();           // reads E820 table from BIOS
    
    // 3. Copy command line
    *cmdline_p = boot_command_line;
    
    // 4. Parse early kernel arguments from cmdline
    parse_early_param();
    ...
}
```

### Phase B: Memory Map Building

The E820 table from BIOS describes all physical memory:
```
E820 Entry Types:
  E820_TYPE_RAM     (1) = Usable RAM
  E820_TYPE_RESERVED(2) = Firmware-reserved (MMIO, ACPI tables)
  E820_TYPE_ACPI    (3) = ACPI reclaimable
  E820_TYPE_NVS     (4) = ACPI NVS (non-volatile storage)
  E820_TYPE_UNUSABLE(5) = Bad RAM (hardware errors)
```

```c
e820__memory_setup();        // read raw BIOS E820 table
e820__end_of_ram_pfn();      // find highest RAM page frame number
e820__reserve_setup_data();  // reserve bootloader-provided data
```

### Phase C: Kernel Image Reservation

```c
// Reserve the kernel itself in memblock
memblock_reserve(__pa_symbol(_text), _end - _text);
// Reserve initrd
memblock_reserve(ramdisk_image, ramdisk_size);
// Reserve crash kernel area (for kdump)
reserve_crashkernel();
```

### Phase D: CPU Feature Detection

```c
early_cpu_init();            // CPUID-based feature detection
    // Reads: vendor, family, model, features
    // Populates: boot_cpu_data (struct cpuinfo_x86)
    // Sets: X86_FEATURE_* bits in boot_cpu_data.x86_capability[]
```

`cpuinfo_x86` has feature words:
```c
struct cpuinfo_x86 {
    __u8    x86;            // CPU family
    __u8    x86_vendor;     // VENDOR_INTEL, VENDOR_AMD, etc.
    __u16   x86_model;
    char    x86_model_id[64];
    __u32   x86_capability[NCAPINTS];  // feature bits from CPUID
    // ...
};
```

CPUID feature flags include:
- `X86_FEATURE_SSE4_2` — SSE 4.2 instructions
- `X86_FEATURE_AVX2` — AVX2 256-bit vector ops
- `X86_FEATURE_RDRAND` — hardware RNG
- `X86_FEATURE_SMEP` — Supervisor Mode Execution Prevention
- `X86_FEATURE_SMAP` — Supervisor Mode Access Prevention
- `X86_FEATURE_PKU` — Memory Protection Keys

### Phase E: Paging Initialization

```c
// Setup the actual kernel page tables
init_mem_mapping();          // map all of physical memory into kernel VA
    //  - Creates direct mapping at __PAGE_OFFSET (0xffff880000000000 on x86-64)
    //  - Maps kernel text/data at -2GB (0xffffffff80000000)

// Finalize memory zones
initmem_init();              // set up NUMA memory nodes from SRAT/SLIT ACPI tables
x86_numa_init();             // or on non-NUMA: UMA init
free_area_init(max_zone_pfns);  // create zone structures
```

### Phase F: ACPI Early Tables

```c
acpi_boot_init();            // parse ACPI MADT (interrupt controller table)
                             // parse ACPI SRAT (system resource affinity = NUMA topology)
                             // parse ACPI SLIT (system locality = NUMA distances)
```

---

## ARM64 `setup_arch()` — Key Differences

```c
// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    // Read DTB (Device Tree Blob) address from x0 register saved at entry
    setup_machine_fdt(__fdt_pointer);
    
    // Parse memory nodes from DT
    early_init_dt_scan_memory();    // instead of E820
    
    // CPU features from ID registers (not CPUID)
    cpuinfo_store_boot_cpu();       // reads ID_AA64PFR0_EL1, etc.
    
    // Paging
    paging_init();
    
    // GIC interrupt controller
    acpi_gic_init() or irqchip_init_dt();
}
```

On ARM (Qualcomm Snapdragon), memory comes from Device Tree nodes:
```dts
/ {
    memory@80000000 {
        device_type = "memory";
        reg = <0x0 0x80000000 0x0 0x40000000>;  // 1GB at 2GB phys
    };
};
```

---

## Sub-Topics (Deep Dive)

- [01_memory_map_detection](01_memory_map_detection/README.md) — E820, memblock, UEFI memory map
- [02_e820_and_memblock](02_e820_and_memblock/README.md) — E820 table format and memblock allocator
- [03_device_tree_parsing](03_device_tree_parsing/README.md) — FDT/DTB format, ARM64 memory detection
- [04_early_ioremap](04_early_ioremap/README.md) — Early I/O memory mapping before vmalloc
- [05_paging_init](05_paging_init/README.md) — Page table setup, direct map, kernel text mapping
- [06_cmdline_parsing](06_cmdline_parsing/README.md) — Kernel command line processing
- [07_x86_specific_cpu_setup](07_x86_specific_cpu_setup/README.md) — CPUID, MSRs, CPU feature bits
- [08_numa_init](08_numa_init/README.md) — NUMA topology, SRAT/SLIT tables, node distances

---

## Interview Q&A

### Q1: What is the E820 memory map and why does the kernel trust it?
**A:** E820 is a BIOS/UEFI interface for reporting physical memory layout. INT 15h, AX=E820h on legacy BIOS, or the `EFI_MEMORY_DESCRIPTOR` array from UEFI `GetMemoryMap()`. The kernel trusts it because it's the authoritative source from firmware about what memory is safe to use. BIOS/UEFI has already reserved memory for ACPI tables, MMIO regions (PCI BAR spaces), SMM (System Management Mode), and option ROMs. If the kernel used those regions, it would corrupt firmware or hardware registers.

### Q2: What is `memblock` and why is it used before the buddy allocator?
**A:** `memblock` is a **boot-time allocator** — a simple list of available and reserved memory regions. It supports `memblock_alloc()` (allocate from available regions) and `memblock_reserve()` (mark a region as reserved). It requires no data structures beyond two linked lists. The buddy allocator (zone-based page allocator) requires page structs, zone structures, and hundreds of KB of metadata — all of which must be allocated from somewhere. `memblock_alloc()` provides that "somewhere". After `mm_core_init()` sets up the buddy allocator using memblock-allocated metadata, memblock's regions are handed off to the buddy allocator and memblock itself is discarded.

### Q3: What does `paging_init()` do and what does the kernel's virtual address space look like afterward?
**A:** `paging_init()` creates the final kernel page tables:
```
x86-64 Kernel Virtual Address Space (4-level paging):
0x0000000000000000 - 0x00007fffffffffff  : User space (128TB)
0xffff800000000000 - 0xffffffffffffffff  : Kernel space (128TB)
  0xffff888000000000 - 0xffffc87fffffffff  : Direct mapping of all physical RAM
  0xffffc90000000000 - 0xffffe8ffffffffff  : vmalloc area
  0xffffe90000000000 - 0xffffe9ffffffffff  : vmemmap (struct page array)
  0xffffffff80000000 - 0xffffffff9fffffff  : kernel text (.text, .data, .rodata)
  0xffffffff00000000 - 0xffffffff7fffffff  : modules area
```
The direct mapping (`__PAGE_OFFSET` = 0xffff888...) maps all physical pages — so `phys_to_virt(pa)` = `pa + 0xffff888000000000`. After `paging_init()`, accessing any physical RAM via its kernel virtual address works correctly.

### Q4: How does NUMA topology affect memory allocation performance?
**A:** On NUMA systems (multi-socket servers at Google/NVIDIA), each CPU has "local" memory (same NUMA node) and "remote" memory (different nodes). Accessing remote memory incurs 2-4x latency due to QPI/UPI/CCIX interconnect traversal. `setup_arch()` reads ACPI SRAT (System Resource Affinity Table) to build a `numa_meminfo` array mapping physical address ranges to NUMA nodes. The buddy allocator then creates per-node zone lists. When a task on CPU 4 (NUMA node 1) calls `kmalloc()`, the allocator first tries node 1's memory — falling back to node 0 only if node 1 is full. This "node-local allocation first" policy is crucial for database performance (e.g., Google Spanner, NVIDIA RAPIDS cuDF).

### Q5: What is KASLR and when is it applied?
**A:** KASLR (Kernel Address Space Layout Randomization) randomizes the physical address where the kernel is loaded. The bootloader (or EFI stub decompressor) reads a random seed from hardware RNG (RDRAND) and adds a random offset (aligned to 2MB) to the kernel's load address. `setup_arch()` then reads where the kernel actually landed and adjusts `_text`, `_data`, etc. symbols accordingly. Without KASLR, kernel address 0xffffffff81000000 is always the same — useful for ROP exploits. With KASLR, an attacker must first find the kernel base via an info-leak, making exploitation much harder.

### Q6: On embedded systems (ARM64, Qualcomm), why is Device Tree used instead of ACPI?
**A:** ACPI requires firmware code (AML bytecode) that is interpreted by the kernel — sophisticated but heavy (200KB+ ACPI subsystem). Device Tree (DT) is a **static data structure** (no code) that describes hardware topology: CPUs, memory, buses, interrupt controllers, clocks, regulators. DT is simpler to implement in bootloaders and is preferred for embedded systems where the hardware is fixed (Qualcomm SoC in a phone). ACPI is preferred for server/PC where hardware is configurable at runtime. ARM64 servers (AWS Graviton, Ampere) use ACPI; ARM64 mobile (Qualcomm, MediaTek) uses DT.

---

## Common Bugs and Pitfalls

| Bug | Description | Fix |
|-----|-------------|-----|
| initrd overwritten | Bootloader places initrd below `min_low_pfn` | Checked later in `start_kernel`, initrd disabled |
| E820 holes | BIOS reports wrong memory ranges | `e820_sanitize()` merges overlapping entries |
| KASLR entropy too low | Boot on old CPUs without RDRAND | Falls back to timing-based entropy from PIT |
| NUMA node count wrong | ACPI SRAT missing or corrupt | Falls back to single-node (UMA) mode |
| cmdline too long | Command line > `COMMAND_LINE_SIZE` (2048) | Truncated silently |
