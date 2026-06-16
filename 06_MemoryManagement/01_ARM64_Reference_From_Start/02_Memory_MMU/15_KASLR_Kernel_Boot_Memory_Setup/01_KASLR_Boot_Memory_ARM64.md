# KASLR, Kernel Boot, and ARM64 Memory Setup

**Category**: KASLR and Kernel Boot Memory Setup  
**Platform**: ARM64 (AArch64)

---

## 1. KASLR (Kernel Address Space Layout Randomization)

```
KASLR: randomize kernel virtual address at boot to defeat info-leak exploits

Without KASLR:
  Kernel always loaded at fixed VA (e.g., 0xFFFF800010000000)
  Attacker knows: gadget addresses, kernel struct offsets
  Exploit: craft precise kernel attack using fixed addresses

With KASLR:
  Kernel VA = fixed_base + random_offset
  random_offset: from EFI RNG service or UEFI GetRNGProtocol
                 or from device tree /chosen/kaslr-seed
                 or from CPU RNDR instruction (ARMv8.5 FEAT_RNG)
  
  ARM64 KASLR range:
    Kernel text: randomized within [0xFFFF800008000000, 0xFFFF800280000000]
    Entropy: ~21 bits (2MB alignment due to section mapping requirement)
    Physical offset also randomized (PHYS_OFFSET randomization)
  
  kimage_voffset:
    = kimage_vaddr - _text_offset
    = VA of kernel text base - physical offset
    Used for: __pa(x), __va(x) macros
    Stored in: arch/arm64/include/asm/memory.h: extern u64 kimage_voffset

ARM64 KASLR implementation:
  arch/arm64/kernel/head.S: choose_kaslr_offset
  Uses EFI_RNG_PROTOCOL if available → 8 bytes of entropy
  Falls back to RNDR instruction (ARMv8.5+)
  Falls back to device tree /chosen/kaslr-seed (set by bootloader)
  
  kaslr_offset(): returns offset applied to virtual addresses
  __pa(vaddr): = (vaddr) - kimage_voffset (if kernel text)
                 = (vaddr) - PAGE_OFFSET (if linear map)
  
  KASLR disabling:
    Boot param: nokaslr
    DEBUG_KASLR=n config disables for debugging
  
  KASLR and kernel modules:
    Module text: randomized within VMALLOC area
    module_alloc(): allocates within ±2GB of kernel text (for relative branch)
    randomized within that 4GB window
  
  KASLR and kallsyms:
    /proc/kallsyms: symbols offset by kaslr offset
    After KASLR: symbol addresses correct in kallsyms
    kptr_restrict: restricts /proc/kallsyms to root (prevents KASLR leak)
```

---

## 2. ARM64 Memory Layout

```
ARM64 kernel virtual address space layout (4KB pages, 48-bit VA):

0x0000000000000000 - 0x0000FFFFFFFFFFFF: User space (TTBR0_EL1)
  [user code, heap, stack, mmap regions]

0xFFFF000000000000 - 0xFFFFFFFFFFFFFFFF: Kernel space (TTBR1_EL1)
  ┌──────────────────────────────────────────────────────────────┐
  │ 0xFFFF000000000000: kernel linear map (PAGE_OFFSET)          │
  │   All physical memory mapped here (direct mapping)           │
  │   Size: up to 128TB or physical memory size                  │
  ├──────────────────────────────────────────────────────────────┤
  │ 0xFFFFA00000000000: vmalloc area (VMALLOC_START)             │
  │   vmalloc(), ioremap(), module text, etc.                    │
  │   Size: ~large range                                         │
  ├──────────────────────────────────────────────────────────────┤
  │ 0xFFFFC00000000000: vmemmap (array of struct page)           │
  │   vmemmap_base: maps all struct page objects                 │
  │   Size: depends on physical memory                           │
  ├──────────────────────────────────────────────────────────────┤
  │ 0xFFFFFF7FFBFF0000: fixmap area                              │
  │   FIXMAP_PAGE_IO, PTE_FIXMAP, etc.                           │
  │   Fixed virtual addresses for early boot use                 │
  ├──────────────────────────────────────────────────────────────┤
  │ 0xFFFFFF8010000000: kernel text/data (with KASLR offset)     │
  │   Randomized within kernel range                             │
  └──────────────────────────────────────────────────────────────┘

ARM64 physical memory layout:
  [0x0000000000000000]: DRAM typically starts here
  [0x0000000040000000]: or 0x80000000 (SoC-dependent)
  DRAM: typically contiguous (multi-NUMA on server, single on embedded)
  MMIO: scattered in PA space, mapped via ioremap() to VMALLOC area

PAGE_OFFSET (Linux kernel config, ARM64):
  Default 48-bit VA: PAGE_OFFSET = 0xFFFF000000000000
  56-bit VA: PAGE_OFFSET = 0xFF00000000000000
  Depends on: CONFIG_ARM64_VA_BITS, CONFIG_ARM64_4K_PAGES etc.
  
  __pa(x) for linear map addresses:
    = x - PAGE_OFFSET + PHYS_OFFSET
    Where PHYS_OFFSET = physical base of RAM
  
  __va(x) for physical addresses:
    = x + PAGE_OFFSET - PHYS_OFFSET
```

---

## 3. memblock: Boot-Time Memory Allocator

```
memblock: first memory allocator before the buddy system is ready

Purpose: manage physical memory during early boot
  Before: slab, buddy, vmalloc are all unavailable
  memblock: simple static structure to track:
    - Available memory regions
    - Reserved memory regions (not for allocation)
  
  struct memblock_region:
    base:  physical base address
    size:  size in bytes
    flags: MEMBLOCK_NONE / MEMBLOCK_HOTPLUG / MEMBLOCK_MIRROR / MEMBLOCK_NOMAP
    nid:   NUMA node ID
  
  struct memblock:
    memory: list of available regions
    reserved: list of reserved regions
  
  Key memblock operations:
    memblock_add(base, size):       add available RAM region
    memblock_reserve(base, size):   mark as reserved (not allocatable)
    memblock_remove(base, size):    remove from available
    memblock_phys_alloc(size, align): allocate from available
    memblock_free(base, size):      return to available
    memblock_is_reserved(addr):     check if addr is reserved
  
  ARM64 boot sequence using memblock:
    1. start_kernel() → setup_arch()
    2. arm64_memblock_init():
       - Parse DT memory nodes (/memory)
       - EFI memory map (if UEFI boot)
       - Add all RAM to memblock.memory
       - Reserve: kernel text/data, initrd, DTB, EFI tables, TZASC regions
       - Reserve: NOMAP regions (CMA, device-specific)
    3. paging_init(): sets up kernel page tables
       - Uses memblock to allocate page table pages
    4. sparse_init() / zone_sizes_init(): set up struct page
       - Uses memblock to allocate struct page arrays
    5. memblock_free_all(): release remaining memblock memory to buddy
  
  NOMAP regions:
    memblock_mark_nomap(base, size): mark region as not mapped in linear map
    Reason: firmware tables, CMA, device reserved regions
    Effect: __va() will not work for these regions; ioremap() needed
    Example: OP-TEE memory: marked NOMAP so Linux doesn't map it
  
  Checking memblock from kernel:
    /sys/kernel/debug/memblock/memory   (debugfs)
    /sys/kernel/debug/memblock/reserved
```

---

## 4. Identity Map and MMU Enable

```
idmap (identity map): required for MMU enable/disable transition

Problem:
  When MMU is off: CPU uses physical addresses
  When MMU turns on: CPU immediately uses virtual addresses
  Code executing the MMU enable instruction must be mapped at:
    VA = PA (identity map) so that the NEXT instruction fetch works

ARM64 identity map:
  arch/arm64/mm/idmap.c
  idmap_pg_dir: page table for identity mappings
  VA = PA for kernel text (with MMU enable/disable code)
  
  __cpu_setup() (arch/arm64/mm/proc.S):
    Setup MAIR_EL1, TCR_EL1, SCTLR_EL1
    Enable MMU: MSR SCTLR_EL1, x0 (sets M bit = 1)
    ISB: immediately after enabling MMU
    This code runs in idmap region: VA = PA → works after MMU enable
    Then: branch to __primary_switch with real kernel VA
  
  CPU hotplug:
    When secondary CPU boots: also needs identity map
    cpu_resume_idmap: secondary CPU entry point in idmap region
    After MMU enable: jumps to __secondary_switched() in normal VA

  Trampoline page:
    KPTI: trampoline_pg_dir = small page table
    Contains: only trampoline code mapped (used during syscall entry/exit)
    Maps: trampoline code at its real VA (not identity)
    Also needed: when switching from user PT to kernel PT

Early page tables (before paging_init):
  init_pg_dir: initial kernel page table (allocated statically or early)
  swapper_pg_dir: final kernel page table
  
  head.S sequence:
    1. Allocate init_pg_dir in .bss or fixup
    2. create_page_tables: map kernel text/data in init_pg_dir
    3. Enable MMU with init_pg_dir loaded in TTBR1_EL1
    4. Jump to start_kernel()
    5. setup_arch() → paging_init() builds proper swapper_pg_dir
    6. Load swapper_pg_dir into TTBR1_EL1
```

---

## 5. EFI Boot and Memory Map

```
EFI (Extensible Firmware Interface) boot on ARM64:

UEFI stub: small piece of code embedded in ARM64 kernel Image
  arch/arm64/kernel/efi-stub.c (and generic drivers/firmware/efi/libstub/)
  UEFI: loads kernel Image file from EFI System Partition
  UEFI stub: calls ExitBootServices(), then jumps to kernel entry

EFI memory map: describes all physical memory and firmware regions
  UEFI provides: EFI_MEMORY_DESCRIPTOR array (from GetMemoryMap())
  Each descriptor:
    Type:         EfiConventionalMemory (usable), EfiReservedMemoryType, ...
    PhysicalStart: base physical address
    NumberOfPages: size in 4KB pages
    Attribute:    EFI_MEMORY_UC, EFI_MEMORY_WB, EFI_MEMORY_RUNTIME, etc.
  
  EFI memory types:
    EfiConventionalMemory: free RAM (add to memblock)
    EfiLoaderCode/Data:    UEFI code/data (free after ExitBootServices)
    EfiBootServicesCode/Data: same, free after ExitBootServices
    EfiRuntimeServicesCode/Data: KEEP: EFI runtime (NVRAM, GetTime, etc.)
    EfiACPIMemory:        ACPI tables (keep until parsed)
    EfiACPIMemoryNVS:     ACPI NVS (keep forever)
    EfiMemoryMappedIO:    MMIO regions
  
  ARM64 Linux EFI handling:
    efi_init() → efi_find_and_save_memmap()
    arm64_memblock_init(): parse EFI memmap → add to memblock
    EFI runtime services: mapped in separate efi_pgd (separate page table)
    efi_virtmap_load(): load EFI virtual memory map
    SetVirtualAddressMap(): tell firmware its new virtual addresses
  
  KASLR and EFI:
    EFI KASLR: UEFI stub calls EFI_RNG_PROTOCOL before ExitBootServices
    Gets random bytes → passes to kernel as /chosen/kaslr-seed or directly
    In memory descriptor: KASLR seed in EFI config table

Device Tree memory:
  /memory node: lists available RAM
    memory@80000000 { device_type = "memory"; reg = <0x0 0x80000000 0x1 0x0>; }
  /reserved-memory: lists reserved regions
    reserved-memory { ranges; OP-TEE@e0000000 { reg = <...>; no-map; }; }
  early_init_dt_scan_memory(): parse /memory nodes → memblock_add
  of_reserved_mem_init_node(): parse /reserved-memory → memblock_reserve
```

---

## 6. Interview Questions & Answers

**Q1: What is kimage_voffset and how does it affect address translation?**
`kimage_voffset` is the difference between the kernel virtual base address and the physical load address: `kimage_voffset = kimage_vaddr - _text_phys`. With KASLR, `kimage_vaddr` is randomized each boot. `__pa(x)` for kernel text = `(unsigned long)(x) - kimage_voffset`. `__va(x)` for kernel PA = `(unsigned long)(x) + kimage_voffset`. For linear map addresses (RAM), different macros apply using PAGE_OFFSET and PHYS_OFFSET. Getting this wrong causes hard-to-debug crashes where everything looks aligned but physical addresses are wrong.

**Q2: Why does ARM64 require an identity map for MMU enable?**
When `SCTLR_EL1.M=1` is written (enabling MMU), the VERY NEXT instruction fetch uses the new page tables. If the code doing the MMU enable is not mapped at VA=PA (identity-mapped), the CPU would try to fetch the next instruction from VA = (old PA) which doesn't exist in the new page tables → prefetch abort. The idmap region maps a small range at VA=PA so that the instruction stream remains valid immediately after MMU enable. Then the code branches to the real kernel virtual address space.

**Q3: What is the memblock allocator and when is it replaced?**
`memblock` is a boot-time physical memory allocator used before the buddy allocator is ready. It maintains two lists: available (`memblock.memory`) and reserved (`memblock.reserved`). ARM64 uses it from `arm64_memblock_init()` until `memblock_free_all()`. During paging setup and struct page allocation, `memblock_phys_alloc()` provides physical pages. After `memblock_free_all()`, all remaining memblock-available pages are donated to the buddy allocator for normal use.

**Q4: How does KASLR interact with kernel symbols and debugging?**
With KASLR, all kernel addresses (text, data, rodata) are shifted by `kaslr_offset`. `/proc/kallsyms` reports correct KASLR-adjusted addresses (since they're computed at runtime). However, `kptr_restrict=2` makes `/proc/kallsyms` show all zeros for non-root users (to prevent KASLR defeat via kallsyms). For debugging: KGDB, crash dump tools, and perf must account for the KASLR offset (apply offset to relocate symbols). `kallsyms_lookup_name()` in kernel returns the correctly offset address.

**Q5: What happens to EFI memory after ExitBootServices()?**
EFI memory types: `EfiLoaderCode/Data` and `EfiBootServicesCode/Data` → freed (returned to available RAM pool). `EfiRuntimeServicesCode/Data` → kept mapped (EFI runtime still callable for: NVRAM read/write, GetTime, UpdateCapsule). `EfiConventionalMemory` → added to memblock as available RAM. ARM64 Linux maps EFI runtime services in a dedicated `efi_pgd` page table and switches to it when calling EFI runtime functions (to avoid contaminating kernel page tables with firmware mappings).

---

## 7. Quick Reference

| Concept | Address Range | Purpose |
|---|---|---|
| User space | 0x0-0xFFFF_0000_0000 | Applications |
| Linear map | PAGE_OFFSET → | All RAM mapped |
| VMALLOC area | VMALLOC_START → | vmalloc(), ioremap |
| vmemmap | vmemmap_base → | struct page array |
| fixmap | FIXADDR_TOP-N → | Early boot fixed VAs |
| Kernel text | ~0xFFFF8001_0000_0000 + KASLR | Kernel code/data |

| Boot Phase | Allocator Used |
|---|---|
| Before start_kernel | memblock (only) |
| paging_init | memblock (alloc page tables) |
| mm_init | buddy (initialized from memblock) |
| After mm_init | slab, vmalloc, buddy |
