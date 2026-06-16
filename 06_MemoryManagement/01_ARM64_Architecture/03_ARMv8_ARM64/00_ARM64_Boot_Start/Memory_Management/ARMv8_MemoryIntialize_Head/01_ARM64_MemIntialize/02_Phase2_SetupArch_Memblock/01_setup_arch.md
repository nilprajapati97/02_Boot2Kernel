# `setup_arch()` — Architecture-Specific Initialization Orchestrator

**Source:** `arch/arm64/kernel/setup.c` lines 280–380
**Phase:** Memblock Era (MMU ON, C code)
**Memory Allocator:** Memblock (becomes available during this function)
**Called by:** `start_kernel()` in init/main.c
**Calls:** Many — this is the master orchestrator for ARM64 hardware discovery

---

## What This Function Does

`setup_arch()` is the **first major C function** that deals with memory. It orchestrates the entire architecture-specific initialization:

1. **Discovers** physical memory from the Device Tree Blob (DTB)
2. **Configures** the memblock allocator with all RAM regions
3. **Creates** the final kernel page tables (linear map of all RAM)
4. **Sets up** memory zones for the buddy allocator

By the end of `setup_arch()`, the kernel knows exactly what physical memory exists and has it all mapped.

---

## How It Works With Memory

### Memory Transitions During setup_arch()

```
BEFORE setup_arch():
  Mapped: Only kernel image (via init_pg_dir)
  Allocator: None
  Knowledge: DTB pointer (x21 from bootloader), nothing else

AFTER setup_arch():
  Mapped: ALL physical RAM (via swapper_pg_dir linear map)
  Allocator: Memblock (all RAM registered, kernel/DTB/initrd reserved)
  Knowledge: Complete — zones, NUMA, DMA limits all determined
```

---

## Step-by-Step Execution

### Step 1: Record Kernel Memory Boundaries

```c
setup_initial_init_mm(_text, _etext, _edata, _end);
```

**What it does:** Records the kernel's virtual address boundaries in `init_mm`:

| Field | Value | Meaning |
|-------|-------|---------|
| `init_mm.start_code` | `_text` | Start of kernel code |
| `init_mm.end_code` | `_etext` | End of kernel code |
| `init_mm.end_data` | `_edata` | End of initialized data |
| `init_mm.brk` | `_end` | End of BSS (end of kernel image) |

`init_mm` is the kernel's `mm_struct` — shared by all kernel threads.

**Memory allocated:** None — `init_mm` is a static global.

---

### Step 2: Early Virtual Address Infrastructure

```c
early_fixmap_init();
early_ioremap_init();
```

**`early_fixmap_init()`:**
- Sets up the **fixmap** — a region of virtual addresses at the top of the kernel VA space
- Each fixmap slot has a **compile-time-determined virtual address**
- Used to map DTB, early console, ACPI tables before the full allocator is ready
- Creates page table entries in `init_pg_dir` for the fixmap region

```
Fixmap VA region (top of kernel space):
┌──────────────────────────┐ FIXADDR_TOP
│ FIX_FDT (DTB mapping)   │ ← One or more 2MB slots
│ FIX_EARLYCON             │ ← Early serial console
│ FIX_TEXT_POKE            │ ← For runtime code patching
│ FIX_BTMAP                │ ← Boot-time ioremap slots
│ ...                      │
└──────────────────────────┘ FIXADDR_START
```

**Memory allocated:** Page table entries in `init_pg_dir` (no memblock yet — uses existing BSS page tables).

**`early_ioremap_init()`:**
- Sets up boot-time I/O memory mapping using fixmap slots
- Provides `early_ioremap()` / `early_iounmap()` for mapping MMIO before vmalloc is available

---

### Step 3: Parse Device Tree — Memory Discovery

```c
setup_machine_fdt(__fdt_pointer);
```

**See:** [02_setup_machine_fdt.md](02_setup_machine_fdt.md) for full details.

**Summary:**
- Maps the DTB at `__fdt_pointer` (physical) via fixmap
- Parses all `/memory` nodes → calls `memblock_add(base, size)` for each bank
- Reserves the DTB itself → `memblock_reserve(dt_phys, dt_size)`
- After this call, **memblock knows about all physical RAM**

**Memory allocated:** Fixmap PTE for DTB mapping (no memblock allocation yet).

---

### Step 4: Early Boot Parameters

```c
jump_label_init();
parse_early_param();
dynamic_scs_init();
```

**`parse_early_param()`:**
- Processes kernel command line parameters
- **Memory-relevant:** `mem=XXX` parameter sets `memory_limit` global
- This limit is enforced later in `arm64_memblock_init()`

---

### Step 5: KASLR and Security

```c
kaslr_init();
```

- Applies Kernel Address Space Layout Randomization
- Adjusts `memstart_addr` for linear map randomization
- This affects the linear map base address

---

### Step 6: CPU and Exception Setup

```c
cpu_uninstall_idmap();
local_daif_restore(DAIF_PROCCTX_NOIRQ);
```

**`cpu_uninstall_idmap()`:**
- Switches TTBR0 from identity map to a **zero page** (trampoline_pg_dir)
- The identity map is no longer needed for the boot CPU
- Prevents speculative instruction fetches from user-space VA range

**`local_daif_restore()`:**
- Unmasks Debug and SError exceptions
- Keeps IRQ/FIQ masked (not ready for interrupts yet)

---

### Step 7: Firmware Detection

```c
xen_early_init();
efi_init();
```

**`efi_init()`:**
- If booted via UEFI, processes EFI memory map
- May add/reserve additional memblock regions from UEFI firmware
- Handles UEFI runtime services memory

---

### Step 8: Memblock Configuration (Critical)

```c
arm64_memblock_init();
```

**See:** [03_arm64_memblock_init.md](03_arm64_memblock_init.md) for full details.

**Summary:**
- Trims memblock to fit within hardware PA limits and kernel VA limits
- Calculates `memstart_addr` (linear map base)
- Reserves: kernel image, initrd, DTB reserved-memory nodes
- After this, memblock is fully configured

**Memory allocated:**
- `memblock_reserve()` calls mark regions as reserved
- No actual allocation — just bookkeeping

---

### Step 9: Page Table Creation (Critical)

```c
paging_init();
```

**See:** [05_paging_init.md](05_paging_init.md) for full details.

**Summary:**
- Creates the **linear map** in `swapper_pg_dir` — maps ALL physical RAM
- Switches TTBR1 from `init_pg_dir` to `swapper_pg_dir`
- After this, any physical address is accessible via the linear map formula:
  `virt = phys - memstart_addr + PAGE_OFFSET`

**Memory allocated:**
- Page table pages (PGD, PUD, PMD, PTE) from memblock
- This is the **first use of memblock as an allocator**

---

### Step 10: Zone and NUMA Setup

```c
bootmem_init();
```

**See:** [07_bootmem_init.md](07_bootmem_init.md) for full details.

**Summary:**
- Calculates `min_low_pfn`, `max_pfn`
- Determines DMA zone boundaries
- Reserves CMA (Contiguous Memory Allocator) regions
- Sets up NUMA topology from DTB/ACPI

**Memory allocated:**
- CMA regions reserved via `memblock_reserve()`
- Crashkernel region reserved if configured

---

### Step 11: KASAN Full Init

```c
kasan_init();
```

- Creates proper shadow memory mappings for KASAN
- Allocates shadow pages from memblock for used memory regions
- Replaces the early zero-page shadow with real shadow pages

**Memory allocated:** Significant — 1/8th of kernel VA space needs shadow pages.

---

### Step 12: CPU Topology

```c
init_bootcpu_ops();
smp_init_cpus();
smp_build_mpidr_hash();
```

- Discovers secondary CPUs from DTB/ACPI
- Builds CPU affinity tables
- Prepares for SMP bringup (secondary CPUs need their own page tables)

---

## Complete Call Sequence (Memory-Relevant Only)

```c
void __init setup_arch(char **cmdline_p)
{
    // 1. Record kernel boundaries
    setup_initial_init_mm(_text, _etext, _edata, _end);

    // 2. Virtual address infrastructure
    early_fixmap_init();          // Fixmap page table entries
    early_ioremap_init();         // Boot-time I/O mapping

    // 3. MEMORY DISCOVERY
    setup_machine_fdt();          // Parse DTB → memblock_add() for each bank

    // 4. Boot parameters
    parse_early_param();          // "mem=" limit

    // 5. MEMBLOCK CONFIGURATION
    arm64_memblock_init();        // Trim, reserve kernel/initrd/DTB

    // 6. PAGE TABLE CREATION
    paging_init();                // Linear map ALL RAM in swapper_pg_dir

    // 7. ZONE SETUP
    bootmem_init();               // Zones, NUMA, CMA, crashkernel

    // 8. KASAN
    kasan_init();                 // Shadow memory allocation
}
```

---

## Memory State After setup_arch()

```
Memblock State:
┌─────────────────────────────────────────────────┐
│ memblock.memory (all RAM):                      │
│   [0x4000_0000 — 0x1_0000_0000]  (3 GB bank)   │
│   [0x8_8000_0000 — 0x9_0000_0000] (2 GB bank)  │
│                                                 │
│ memblock.reserved (claimed regions):            │
│   [kernel_start — kernel_end]     kernel image  │
│   [dtb_start — dtb_end]           Device Tree   │
│   [initrd_start — initrd_end]     initrd        │
│   [cma_start — cma_end]           CMA region    │
│   [crashkernel region]            if configured  │
│   [page tables]                   swapper_pg_dir │
└─────────────────────────────────────────────────┘

Page Tables: swapper_pg_dir (TTBR1_EL1)
  Linear map: ALL memblock.memory → kernel VA space
  Kernel image: mapped with proper permissions

Zones Defined:
  ZONE_DMA:    [min_pfn — dma_limit_pfn]
  ZONE_DMA32:  [min_pfn — 0x1_0000 (4GB)]
  ZONE_NORMAL: [0x1_0000 — max_pfn]
```

---

## Key Takeaways

1. **setup_arch() is the bridge** — from "kernel image only" to "all RAM mapped and known"
2. **Memblock becomes usable during this function** — after `setup_machine_fdt()` adds RAM banks
3. **Three critical sub-functions:** `arm64_memblock_init()` → `paging_init()` → `bootmem_init()`
4. **Order matters** — DTB must be parsed before memblock config, memblock must be ready before page tables, page tables must exist before zone setup
5. **`swapper_pg_dir` replaces `init_pg_dir`** — the temporary boot page tables are replaced with the final ones
