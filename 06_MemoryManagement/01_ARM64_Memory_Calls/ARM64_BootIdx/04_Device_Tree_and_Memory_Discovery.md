# Layer 4 — Device Tree & Memory Discovery

[← Layer 3: Kernel Virtual Mapping](03_Kernel_Virtual_Mapping.md) | **Layer 4** | [Layer 5: Memblock Early Allocator →](05_Memblock_Early_Allocator.md)

---

## What This Layer Covers

The kernel is now running at its virtual address, but it has no idea how much physical
RAM exists or where it is. This layer explains how `start_kernel()` calls into
`setup_arch()`, which parses the Flattened Device Tree (FDT) to discover physical memory
regions and register them with the `memblock` allocator. After this layer, the kernel
knows exactly what physical memory is available.

---

## 1. `start_kernel()` — The C Entry Point

**Source**: `init/main.c`

`start_kernel()` is the first C function called after the assembly boot code. Here is
the memory-related call sequence (non-memory calls omitted):

```
start_kernel()                                    [init/main.c]
    │
    ├──► page_address_init()                      Init highmem page hash
    │
    ├──► setup_arch(&command_line)  ★             Architecture-specific init
    │    (ARM64 memory discovery happens here)     [arch/arm64/kernel/setup.c]
    │    (Explained in detail in Section 2 below)
    │
    ├──► mm_core_init_early()                     Zones & nodes init
    │    └─ free_area_init()                      (Layer 7)
    │
    ├──► setup_per_cpu_areas()                    Per-CPU memory regions
    │
    ├──► early_numa_node_init()                   NUMA early init
    │
    ├──► setup_log_buf(0)                         Allocate kernel log buffer
    │                                              (uses memblock_alloc)
    │
    ├──► mm_core_init()            ★              Buddy + Slab + vmalloc
    │    (Layer 8 and 9)                           [mm/mm_init.c]
    │
    ├──► kmem_cache_init_late()                   Finalize SLUB
    │
    ├──► setup_per_cpu_pageset()                  Per-CPU page caches
    │
    ├──► numa_policy_init()                       NUMA memory policy
    │
    ├──► anon_vma_init()                          Anonymous VMA slab cache
    ├──► fork_init()                              task_struct slab cache
    ├──► proc_caches_init()                       Process-related slab caches
    ├──► vfs_caches_init()                        VFS slab caches (dentry, inode)
    │
    └──► rest_init()                              Spawns init process
```

---

## 2. `setup_arch()` — ARM64 Memory Discovery

**Source**: `arch/arm64/kernel/setup.c`

This is where the ARM64 kernel discovers its hardware. The memory-related calls in order:

```
setup_arch(char **cmdline_p)
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE A: Set up the init_mm (kernel's mm_struct)       │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► setup_initial_init_mm(_text, _etext, _edata, _end)
    │       Sets init_mm boundaries:
    │         start_code  = _text
    │         end_code    = _etext
    │         end_data    = _edata
    │         brk         = _end
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE B: Set up early virtual mapping infrastructure   │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► early_fixmap_init()           ──► Section 3
    ├──► early_ioremap_init()          ──► Section 4
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE C: Parse Device Tree — DISCOVER MEMORY ★         │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► setup_machine_fdt()           ──► Section 5 ★
    │       Map FDT via fixmap, parse /memory, /chosen, /reserved-memory
    │       → memblock_add() for each memory region
    │
    ├──► parse_early_param()           Process "mem=", "memmap=" etc.
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE D: Clean up address spaces                       │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► cpu_uninstall_idmap()         Remove TTBR0 identity map
    │       Set TTBR0 to reserved_pg_dir (empty)
    │       Now ONLY TTBR1 (kernel) mappings are active.
    │       Any TTBR0 (user-space) access will fault.
    │
    ├──► efi_init()                    EFI memory map (if UEFI boot)
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE E: Configure memblock with architecture limits   │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► arm64_memblock_init()         ──► Layer 5 (dedicated document)
    │       Clip memblock regions, reserve kernel, initrd, etc.
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE F: Create final page tables                      │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► paging_init()                 ──► Layer 6 (dedicated document)
    │       Create linear map of ALL RAM in swapper_pg_dir
    │
    ├──► unflatten_device_tree()       Full DT parsing (struct device_node tree)
    │
    │   ┌────────────────────────────────────────────────────────┐
    │   │ PHASE G: Set up NUMA nodes and memory zones            │
    │   └────────────────────────────────────────────────────────┘
    │
    ├──► bootmem_init()                ──► Layer 7 (dedicated document)
    │       NUMA init, zone limits, CMA, crashkernel
    │
    ├──► kasan_init()                  KASAN shadow memory
    │
    └──► request_standard_resources()  Register RAM as iomem resource
```

---

## 3. `early_fixmap_init()` — Fixed Virtual Mappings

**Source**: `arch/arm64/mm/fixmap.c`

### 3.1 What is the Fixmap?

The fixmap is a region of virtual addresses near the top of the kernel's VA space
(around `0xFFFF_FFFF_FF80_0000`) that provide **compile-time-constant virtual addresses**
for temporary mappings. Unlike normal virtual addresses, fixmap addresses are known at
compile time, so the kernel can use them before any dynamic allocator exists.

### 3.2 Why It's Needed Now

The kernel needs to read the Device Tree (FDT) to discover memory. But the FDT is at
a physical address (passed in x0 by the bootloader), and the linear map doesn't exist
yet. The fixmap provides a way to create a temporary virtual mapping of the FDT's
physical address.

### 3.3 What `early_fixmap_init()` Does

```
early_fixmap_init()
    │
    │  The fixmap region is at FIXADDR_TOT_START → FIXADDR_TOP
    │  (near the top of the kernel VA space)
    │
    ├──► Walk swapper_pg_dir from PGD → P4D → PUD
    │    using physical addresses (pgd_offset_k, p4d_offset_kimg)
    │
    └──► early_fixmap_init_pud()
         └─► Create PUD → PMD → PTE entries for the fixmap range
             These page table pages come from the kernel's BSS
             section (bm_pud, bm_pmd, bm_pte — static arrays).

Result:
    The page table hierarchy for FIXADDR_TOT_START → FIXADDR_TOP
    exists in swapper_pg_dir. Individual fixmap PTEs can now be
    set/cleared to map/unmap physical addresses on the fly.
```

### 3.4 Fixmap Slots

The fixmap provides named slots (indices) for different uses:

```
Fixmap Indices (partial list)
═════════════════════════════

Index                    Purpose
─────                    ───────
FIX_FDT                  Device Tree Blob mapping (multiple pages)
FIX_EARLYCON_MEM_BASE    Early serial console MMIO
FIX_TEXT_POKE0           Code patching (alternatives)
FIX_BTMAP_BEGIN/END       Early ioremap slots
FIX_PTE/PMD/PUD/PGD      Temporary page table manipulation
```

---

## 4. `early_ioremap_init()` — Early I/O Remapping

**Source**: `arch/arm64/mm/ioremap.c`

This initializes the early ioremap subsystem, which provides a small number of temporary
MMIO mapping slots built on top of the fixmap. These are used before `ioremap()` is
available (ioremap needs vmalloc, which needs slab, which needs buddy — none of which
exist yet).

```
early_ioremap_init()
    └─► early_ioremap_setup()       [lib/early_ioremap.c]
        Initialize FIX_BTMAP_BEGIN..FIX_BTMAP_END slots.
        Provides early_ioremap() / early_iounmap() functions.
        Typically 7 slots available for temporary MMIO mappings.
```

---

## 5. `setup_machine_fdt()` — Parsing the Device Tree for Memory

**Source**: `arch/arm64/kernel/setup.c`

This is where the kernel first discovers how much physical RAM the board has.

### 5.1 Mapping the FDT

```
setup_machine_fdt(__fdt_pointer)
    │
    ├──► fixmap_remap_fdt(dt_phys)
    │       Map the FDT physical address into the fixmap VA region.
    │       Uses the FIX_FDT fixmap slot.
    │       Now the kernel can read the FDT via its fixmap VA.
    │
    ├──► Validate FDT header
    │       Check magic number, version, structure.
    │
    ├──► memblock_reserve(dt_phys, fdt_totalsize)
    │       Reserve the FDT's physical memory so nothing
    │       overwrites it during boot.
    │
    └──► early_init_dt_scan(fdt)     ★ THE KEY CALL ★
            Parse the FDT for memory, chosen, reserved-memory.
```

### 5.2 `early_init_dt_scan()` → `early_init_dt_scan_nodes()`

**Source**: `drivers/of/fdt.c`

```
early_init_dt_scan(fdt)
    │
    └──► early_init_dt_scan_nodes()
         │
         ├──► early_init_dt_scan_chosen(boot_command_line)
         │       Parse /chosen node:
         │       ┌─────────────────────────────────────────┐
         │       │ • Extract "bootargs" → kernel cmdline   │
         │       │ • Extract "linux,initrd-start"          │
         │       │ • Extract "linux,initrd-end"            │
         │       │ • Extract "stdout-path" for console     │
         │       └─────────────────────────────────────────┘
         │
         ├──► early_init_dt_scan_memory()      ★ MEMORY DISCOVERY ★
         │       (Explained in detail below)
         │
         ├──► early_init_dt_check_for_usable_mem_range()
         │       For crashdump kernels: limit usable memory
         │
         └──► early_init_dt_check_kho()
                 Kexec handover support
```

### 5.3 `early_init_dt_scan_memory()` — The Moment RAM is Discovered

**Source**: `drivers/of/fdt.c`

This function iterates every node in the flattened device tree, looking for nodes with
`device_type = "memory"`, and registers their address ranges with memblock:

```
early_init_dt_scan_memory()
    │
    │   For each node in the FDT:
    │
    ├──► Check: does node have device_type = "memory"?
    │       If not, skip this node.
    │
    ├──► Try to read "linux,usable-memory" property
    │       (Used by crash dump kernels to limit memory range)
    │
    ├──► If not found, read "reg" property
    │       This contains (base, size) pairs describing physical RAM.
    │
    │   For each (base, size) pair in the reg property:
    │
    ├──► early_init_dt_add_memory_arch(base, size)
    │    │
    │    ├── Validate: base must be page-aligned
    │    │   (trim if not)
    │    │
    │    ├── Validate: must not overflow physical address space
    │    │
    │    └── memblock_add(base, size)          ★ REGISTER RAM ★
    │           Add this physical memory range to memblock.memory
    │
    └──► If node is marked "hotpluggable":
            memblock_mark_hotplug(base, size)
```

### 5.4 Concrete Example

Given this Device Tree:

```
memory@40000000 {
    device_type = "memory";
    reg = <0x00 0x40000000 0x00 0x80000000>;   /* 2 GB at 0x40000000 */
};

memory@100000000 {
    device_type = "memory";
    reg = <0x01 0x00000000 0x01 0x00000000>;   /* 4 GB at 0x100000000 */
};
```

The parsing results in:

```
memblock.memory after parsing:
═══════════════════════════════

Region 0:  base = 0x0000_0000_4000_0000  size = 0x8000_0000 (2 GB)
Region 1:  base = 0x0000_0001_0000_0000  size = 0x1_0000_0000 (4 GB)

Total known RAM: 6 GB

memblock.reserved at this point:
════════════════════════════════

Region 0:  FDT blob (reserved by setup_machine_fdt)
```

---

## 6. What Happens After Memory Discovery

After `setup_machine_fdt()` returns, the kernel knows about physical RAM but still needs
to do more work:

```
After setup_machine_fdt() returns to setup_arch():
    │
    ├──► parse_early_param()
    │       Process early command line parameters:
    │       • "mem=512M" — limit usable memory to 512 MB
    │       • "memmap=..." — override memory map
    │       • "hugepages=..." — reserve huge pages
    │       These may MODIFY memblock regions.
    │
    ├──► cpu_uninstall_idmap()     ★ Remove identity map
    │       TTBR0 = reserved_pg_dir (empty)
    │       The identity map is no longer needed.
    │       From now on, only kernel VA (TTBR1) is active.
    │
    ├──► efi_init()
    │       If booted via UEFI: process EFI memory map.
    │       May add/modify memblock regions based on UEFI
    │       memory descriptors.
    │
    └──► arm64_memblock_init()     → Layer 5
            Fine-tune memblock: clip to VA/PA limits,
            reserve kernel image, initrd, DT reserved-memory.
```

---

## 7. Fixmap: How the FDT Mapping Works (Detail)

Since the linear map doesn't exist yet, the kernel uses a clever trick to access the
FDT:

```
Step 1: Bootloader puts FDT at physical address 0x4800_0000
        Kernel receives this in x0 → saved as __fdt_pointer

Step 2: early_fixmap_init() creates page table entries for
        the fixmap region (around 0xFFFF_FFFF_FF80_0000)

Step 3: fixmap_remap_fdt(0x4800_0000):
        ├─ Compute fixmap VA for FIX_FDT slot
        │  (e.g., 0xFFFF_FFFF_FFA0_0000)
        │
        ├─ Write PTE in swapper_pg_dir's fixmap region:
        │  PTE = 0x4800_0000 | PAGE_KERNEL (RW, NX, cacheable)
        │
        └─ Return virtual address 0xFFFF_FFFF_FFA0_0000

Step 4: Kernel reads FDT via 0xFFFF_FFFF_FFA0_0000
        MMU translates: VA 0xFFFF_FFFF_FFA0_0000 → PA 0x4800_0000
        FDT contents are now readable!
```

```
Physical Memory:                        Kernel Virtual Address Space:

0x4800_0000 ┌──────────┐               0xFFFF_FFFF_FFA0_0000 ┌──────────┐
            │          │                                      │          │
            │   FDT    │◄───── MMU translation ──────────────│   FDT    │
            │ (Device  │               (fixmap PTE)           │ (fixmap  │
            │  Tree)   │                                      │  slot)   │
            │          │                                      │          │
0x4800_8000 └──────────┘               0xFFFF_FFFF_FFA0_8000 └──────────┘
```

---

## 8. State After Layer 4

```
┌────────────────────────────────────────────────────────────────┐
│              STATE AFTER DEVICE TREE PARSING                    │
│                                                                │
│  Memory Knowledge:                                             │
│    ✓ memblock.memory contains ALL physical RAM regions          │
│      (discovered from /memory DT nodes)                        │
│    ✓ memblock.reserved contains FDT reservation                │
│    ✓ Kernel command line parsed (may have mem= limit)           │
│                                                                │
│  Identity Map:                                                 │
│    ✗ REMOVED (cpu_uninstall_idmap called)                       │
│    TTBR0 = reserved_pg_dir (empty, any user access faults)     │
│                                                                │
│  Fixmap:                                                       │
│    ✓ Active — FDT mapped via fixmap, accessible                │
│                                                                │
│  Page Tables:                                                  │
│    ✓ swapper_pg_dir has kernel image + fixmap entries           │
│    ✗ Linear map NOT created yet                                │
│    ✗ VMALLOC/VMEMMAP NOT set up yet                            │
│                                                                │
│  Memory Allocator:                                             │
│    ✓ memblock is FUNCTIONAL (can allocate/reserve)             │
│      (memblock_alloc works via memblock.memory - .reserved)    │
│    ✗ Buddy allocator NOT ready                                 │
│    ✗ Slab allocator NOT ready                                  │
│                                                                │
│  What Comes Next:                                              │
│    → arm64_memblock_init (Layer 5): fine-tune memblock          │
│    → paging_init (Layer 6): create linear map of all RAM       │
│    → free_area_init (Layer 7): set up zones and struct page    │
│    → memblock_free_all (Layer 8): hand off to buddy            │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 4

| Step | Function | What It Did |
|------|----------|-------------|
| 1 | `early_fixmap_init()` | Created page table entries for fixmap VA region |
| 2 | `early_ioremap_init()` | Set up temporary MMIO mapping slots on fixmap |
| 3 | `fixmap_remap_fdt()` | Mapped FDT physical address into fixmap VA |
| 4 | `memblock_reserve(fdt)` | Reserved FDT memory from being overwritten |
| 5 | `early_init_dt_scan_chosen()` | Parsed /chosen: command line, initrd location |
| 6 | `early_init_dt_scan_memory()` | **Parsed /memory nodes → `memblock_add()` for each RAM region** |
| 7 | `parse_early_param()` | Processed `mem=` and other memory cmdline params |
| 8 | `cpu_uninstall_idmap()` | Removed identity map from TTBR0 |

**The kernel now knows about ALL physical RAM on the board**, stored in `memblock.memory`.
The next step (Layer 5) fine-tunes this information and reserves critical regions.

---

[← Layer 3: Kernel Virtual Mapping](03_Kernel_Virtual_Mapping.md) | **Layer 4** | [Layer 5: Memblock Early Allocator →](05_Memblock_Early_Allocator.md)
