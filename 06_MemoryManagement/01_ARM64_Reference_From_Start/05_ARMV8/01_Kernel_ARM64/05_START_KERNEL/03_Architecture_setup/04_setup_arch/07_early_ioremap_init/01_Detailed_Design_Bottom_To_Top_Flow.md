# early_ioremap_init() — Detailed Design: Bottom-to-Top Flow
# Complete Interview-Ready Deep Dive

---

## TABLE OF CONTENTS

1. [The Problem This Function Solves](#1-the-problem-this-function-solves)
2. [Fundamental Concepts (The Foundation)](#2-fundamental-concepts-the-foundation)
3. [Hardware Reality: The MMU Before Paging](#3-hardware-reality-the-mmu-before-paging)
4. [The Fixmap Subsystem](#4-the-fixmap-subsystem)
5. [Bottom Layer: early_ioremap_setup()](#5-bottom-layer-early_ioremap_setup)
6. [Middle Layer: early_ioremap_init() Itself](#6-middle-layer-early_ioremap_init-itself)
7. [Top Layer: How the Kernel Uses It](#7-top-layer-how-the-kernel-uses-it)
8. [Complete Call Flow Diagram](#8-complete-call-flow-diagram)
9. [Memory Layout Diagram](#9-memory-layout-diagram)
10. [Data Structures Deep Dive](#10-data-structures-deep-dive)
11. [Page Table Walk During early_ioremap](#11-page-table-walk-during-early_ioremap)
12. [Hardware & Kernel Reaction Step by Step](#12-hardware--kernel-reaction-step-by-step)
13. [Lifecycle: From Boot to Paging Init](#13-lifecycle-from-boot-to-paging-init)
14. [Interview Questions and Answers](#14-interview-questions-and-answers)

---

## 1. The Problem This Function Solves

### The Core Challenge

When the Linux kernel boots, it faces a fundamental chicken-and-egg problem:

```
PROBLEM:
  - Kernel needs to READ hardware registers and memory-mapped devices
    (device tree blob, UART, interrupt controller, etc.)
  - Reading these requires virtual addresses (pointer dereferencing in C)
  - Virtual addresses require the MMU to be active with page tables
  - Page tables require memory allocation
  - Memory allocation requires knowing what memory exists
  - Knowing memory requires reading device tree or ACPI tables
  - Reading device tree requires virtual addresses  ← LOOP
```

**early_ioremap_init()** breaks this deadlock by providing a **small, temporary virtual address window** that can map physical addresses to virtual addresses BEFORE the full virtual memory system (`vmalloc`, `ioremap`) is online.

### What Happens Without It

Without `early_ioremap_init()`, the kernel cannot:
- Read the Flattened Device Tree (FDT) blob passed by the bootloader
- Access UART for early console output (earlycon)
- Read ACPI tables on supported ARM platforms
- Parse memory layout information
- Access the interrupt controller configuration

---

## 2. Fundamental Concepts (The Foundation)

### 2.1 Physical vs Virtual Address Space

```
PHYSICAL ADDRESS SPACE (Real Hardware):
┌─────────────────────────────────────┐ 0xFFFFFFFF
│   MMIO Registers (UART, GIC, etc.) │
├─────────────────────────────────────┤
│   RAM                               │
│   (kernel image loaded here)        │
├─────────────────────────────────────┤
│   ROM/Flash                         │
└─────────────────────────────────────┘ 0x00000000

VIRTUAL ADDRESS SPACE (CPU sees after MMU ON):
┌─────────────────────────────────────┐ 0xFFFFFFFF (ARM32)
│   Fixmap Region   ← THAT IS US      │
├─────────────────────────────────────┤ 0xFFC00000
│   vmalloc / ioremap area            │
├─────────────────────────────────────┤
│   Kernel direct-mapped RAM          │
├─────────────────────────────────────┤
│   User space                        │
└─────────────────────────────────────┘ 0x00000000
```

### 2.2 What is ioremap?

`ioremap()` is the normal kernel function to map physical I/O memory into virtual address space. But `ioremap()` requires:
- `vmalloc_init()` to be complete
- The slab allocator or memblock to allocate page table pages
- Full page table management infrastructure

**All of this comes AFTER `setup_arch()` completes.** So for the early boot phase, we need `early_ioremap()`.

### 2.3 What is a Fixmap?

A fixmap is a **compile-time allocated virtual address**. The kernel reserves specific virtual address slots at compile time. At runtime, the kernel can "plug in" any physical address into these pre-allocated virtual slots.

```c
// From arch/arm/include/asm/fixmap.h
enum fixed_addresses {
    FIX_EARLYCON_MEM_BASE,        // Slot for early console
    __end_of_permanent_fixed_addresses,
    FIX_KMAP_BEGIN,               // kmap region (shared with btmap)
    ...
    FIX_BTMAP_END,                // Boot-time temporary map end
    FIX_BTMAP_BEGIN,              // Boot-time temporary map begin
};
```

The "btmap" (boot-time map) entries are the ones `early_ioremap_init()` manages.

---

## 3. Hardware Reality: The MMU Before Paging

### 3.1 The Boot Sequence Leading to early_ioremap_init

```
BOOTLOADER (U-Boot / EFI):
  1. Places kernel image at physical address (e.g., 0x80008000 on ARM32)
  2. Passes r0=0, r1=machine_id, r2=pointer_to_atags_or_dtb
  3. Jumps to kernel entry point

KERNEL HEAD.S (assembly):
  4. Sets up a minimal identity-mapped page table
     - Maps the kernel image: phys == virt (identity map)
     - Maps a few more pages needed immediately
  5. Enables MMU (writes to CP15 SCTLR on ARM32)
     - After this instruction, all memory accesses go through MMU

KERNEL start_kernel() (C code now running with MMU ON):
  6. setup_arch() is called
  7. early_fixmap_init() sets up fixmap page table entries
  8. early_ioremap_init() sets up the virtual address slots ← WE ARE HERE
```

### 3.2 The MMU State When early_ioremap_init() Runs

At the point `early_ioremap_init()` is called, the MMU is **ON** but only has a minimal page table:

```
PAGE TABLE STATE at this point (ARM32):
┌────────────────────────────────────────────────────────┐
│ Page Global Directory (pgd) - 4KB, 2048 entries        │
├────────────────────────────────────────────────────────┤
│ Entry 0:     kernel identity map (phys 0x80000000)     │
│ Entry 1:     kernel virtual map  (virt 0xC0000000)     │
│ Entry 0xFFC: fixmap PMD (just set by early_fixmap_init)│
│ All others:  INVALID / UNMAPPED                        │
└────────────────────────────────────────────────────────┘
```

**Critical:** The fixmap PMD entry exists in the page table (set by `early_fixmap_init()`), but the PTE entries within that PMD are all **empty/unmapped**. `early_ioremap_init()` initializes the software bookkeeping to MANAGE those PTE entries.

---

## 4. The Fixmap Subsystem

### 4.1 Virtual Address Calculation

The fixmap uses a mathematical formula to convert an index to a virtual address:

```c
// From include/asm-generic/fixmap.h
#define __fix_to_virt(x)   (FIXADDR_TOP - ((x) << PAGE_SHIFT))
```

For ARM32:
- `FIXADDR_TOP = 0xFFF00000`
- Each index is one page (4KB = 0x1000) apart
- Index 0 → `0xFFF00000 - 0 = 0xFFF00000`  (but actually FIXADDR_TOP - PAGE_SIZE)
- Index 1 → `0xFFF00000 - 0x1000 = 0xFFEFF000`
- Index N → `0xFFF00000 - N * 0x1000`

### 4.2 Boot-Time Map (btmap) Layout

The btmap region is the portion of fixmap reserved for early_ioremap:

```c
// ARM32 fixmap.h
#define NR_FIX_BTMAPS       32    // 32 pages per slot = 128KB per slot
#define FIX_BTMAPS_SLOTS     7    // 7 concurrent mappings max
#define TOTAL_FIX_BTMAPS    (NR_FIX_BTMAPS * FIX_BTMAPS_SLOTS)  // 224 pages = 896KB

FIX_BTMAP_END   = __end_of_permanent_fixed_addresses
FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1
```

Visual Layout:
```
FIXMAP VIRTUAL ADDRESS SPACE (top of virtual memory):
                                                    0xFFF00000 (FIXADDR_TOP)
┌──────────────────────────────────────────────────┐
│  Permanent Fixed Addresses:                      │
│    FIX_EARLYCON_MEM_BASE                         │
│    FIX_KMAP_BEGIN..FIX_KMAP_END                  │
│    FIX_TEXT_POKE0, FIX_TEXT_POKE1                │
│    __end_of_permanent_fixed_addresses            │
├──────────────────────────────────────────────────┤ ← FIX_BTMAP_END
│  SLOT 0:  32 pages (128KB) for early_ioremap     │
│  SLOT 1:  32 pages (128KB) for early_ioremap     │
│  SLOT 2:  32 pages (128KB) for early_ioremap     │
│  SLOT 3:  32 pages (128KB) for early_ioremap     │
│  SLOT 4:  32 pages (128KB) for early_ioremap     │
│  SLOT 5:  32 pages (128KB) for early_ioremap     │
│  SLOT 6:  32 pages (128KB) for early_ioremap     │
└──────────────────────────────────────────────────┘ ← FIX_BTMAP_BEGIN
                                            ~0xFFC80000 (FIXADDR_START ARM32)
```

---

## 5. Bottom Layer: early_ioremap_setup()

This is the actual work-horse called by `early_ioremap_init()`.

### 5.1 Source Code

```c
// mm/early_ioremap.c

static void __iomem *prev_map[FIX_BTMAPS_SLOTS] __initdata;
static unsigned long prev_size[FIX_BTMAPS_SLOTS] __initdata;
static unsigned long slot_virt[FIX_BTMAPS_SLOTS] __initdata;

void __init early_ioremap_setup(void)
{
    int i;

    for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
        WARN_ON_ONCE(prev_map[i]);
        slot_virt[i] = __fix_to_virt(FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*i);
    }
}
```

### 5.2 What This Does Step by Step

**Step 1:** The function iterates over all 7 slots.

**Step 2:** For each slot, it calls `WARN_ON_ONCE(prev_map[i])`.
- `prev_map[i]` tracks if a mapping is currently active in slot `i`
- At init time, all should be NULL (no mappings yet)
- If not NULL, something went wrong — emit a kernel warning

**Step 3:** Computes and stores the base virtual address for each slot:

```
Slot 0: slot_virt[0] = __fix_to_virt(FIX_BTMAP_BEGIN - 0)
       = __fix_to_virt(FIX_BTMAP_BEGIN)
       = FIXADDR_TOP - FIX_BTMAP_BEGIN * PAGE_SIZE

Slot 1: slot_virt[1] = __fix_to_virt(FIX_BTMAP_BEGIN - 32)
       = FIXADDR_TOP - (FIX_BTMAP_BEGIN - 32) * PAGE_SIZE
       = slot_virt[0] + 32 * PAGE_SIZE  (128KB higher)

Slot 2: slot_virt[2] = __fix_to_virt(FIX_BTMAP_BEGIN - 64)
...
```

**The key insight:** Each slot's virtual address base is 32 pages (128KB) apart. Each slot can map up to 128KB of physical memory at a time.

### 5.3 State After early_ioremap_setup()

```
SOFTWARE STATE:
prev_map[0..6]  = { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
prev_size[0..6] = { 0, 0, 0, 0, 0, 0, 0 }
slot_virt[0..6] = {
    0xFFxxxxx0,   // Slot 0 base virtual address
    0xFFxxxxx0,   // Slot 1 base (128KB higher than slot 0)
    0xFFxxxxx0,   // Slot 2 base (128KB higher than slot 1)
    ...
}

HARDWARE PAGE TABLE STATE:
- The fixmap PMD is wired up (done by early_fixmap_init())
- But ALL PTE entries in the fixmap region are EMPTY (no physical mapping)
- Any access to these virtual addresses → PAGE FAULT (hardware exception)
```

---

## 6. Middle Layer: early_ioremap_init() Itself

### 6.1 ARM32 Source

```c
// arch/arm/mm/ioremap.c

/*
 * Must be called after early_fixmap_init
 */
void __init early_ioremap_init(void)
{
    early_ioremap_setup();
}
```

For ARM32, this is just a thin wrapper. The comment is critical: **must be called after `early_fixmap_init()`**. This is because:
- `early_fixmap_init()` sets up the **hardware page table entries** for the fixmap PMD
- `early_ioremap_init()` initializes the **software bookkeeping** (slot_virt array)

Without `early_fixmap_init()` first, calling `__early_set_fixmap()` later would crash because there is no PMD entry to hold the PTEs.

### 6.2 The __init Attribute

```c
void __init early_ioremap_init(void)
```

`__init` means:
- This function is placed in the `.init.text` section of the kernel binary
- After the kernel finishes initialization (`free_initmem()`), this section is freed
- The virtual memory pages that held this code become available for other uses
- If called after init, it would cause a kernel oops (page not present)

### 6.3 Calling Context in setup_arch()

```c
// arch/arm/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    ...
    early_fixmap_init();     // Step 1: Set up page table infrastructure
    early_ioremap_init();    // Step 2: Initialize software slot tracking  ← HERE
    
    parse_early_param();     // Step 3: Now we can use early_ioremap()!
    ...
    early_ioremap_reset();   // Step 4: Mark as "paging initialized" after paging_init()
    paging_init(mdesc);      // Step 5: Full page tables created
    ...
}
```

---

## 7. Top Layer: How the Kernel Uses It

### 7.1 early_ioremap() Implementation

After `early_ioremap_init()`, the kernel can call:

```c
// mm/early_ioremap.c
void __init __iomem *
early_ioremap(resource_size_t phys_addr, unsigned long size)
{
    return __early_ioremap(phys_addr, size, FIXMAP_PAGE_IO);
    // FIXMAP_PAGE_IO = device memory type (non-cacheable, non-bufferable)
}
```

### 7.2 __early_ioremap() Full Implementation Walkthrough

```c
static void __init __iomem *
__early_ioremap(resource_size_t phys_addr, unsigned long size, pgprot_t prot)
{
    unsigned long offset;
    resource_size_t last_addr;
    unsigned int nrpages;
    enum fixed_addresses idx;
    int i, slot;

    // SAFETY CHECK: Should not be called after system is running
    WARN_ON(system_state >= SYSTEM_RUNNING);

    // STEP 1: Find a free slot
    slot = -1;
    for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
        if (!prev_map[i]) {
            slot = i;
            break;
        }
    }
    if (WARN(slot < 0, "no slot found")) return NULL;

    // STEP 2: Validate size (no zero, no wraparound)
    last_addr = phys_addr + size - 1;
    if (WARN_ON(!size || last_addr < phys_addr)) return NULL;
    prev_size[slot] = size;

    // STEP 3: Align to page boundary
    offset = offset_in_page(phys_addr);   // byte offset within first page
    phys_addr &= PAGE_MASK;               // round down to page boundary
    size = PAGE_ALIGN(last_addr + 1) - phys_addr;  // round up

    // STEP 4: Check fit in slot
    nrpages = size >> PAGE_SHIFT;
    if (WARN_ON(nrpages > NR_FIX_BTMAPS)) return NULL;

    // STEP 5: Install the page table entries
    idx = FIX_BTMAP_BEGIN - NR_FIX_BTMAPS * slot;
    while (nrpages > 0) {
        __early_set_fixmap(idx, phys_addr, prot);
        // This writes a PTE: virt(idx) → phys_addr with permissions 'prot'
        phys_addr += PAGE_SIZE;
        --idx;      // Move to next fixmap entry (lower virtual address)
        --nrpages;
    }

    // STEP 6: Record the mapping and return virtual address
    prev_map[slot] = (void __iomem *)(offset + slot_virt[slot]);
    return prev_map[slot];
    // Returns the virtual address corresponding to phys_addr + original offset
}
```

### 7.3 __early_set_fixmap() on ARM32

```c
// arch/arm/include/asm/fixmap.h
#define __early_set_fixmap    __set_fixmap

// arch/arm/mm/mmu.c
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
    unsigned long vaddr = __fix_to_virt(idx);
    pte_t *pte = pte_offset_fixmap(pmd_off_k(vaddr), vaddr);

    if (pgprot_val(prot))
        set_pte_at(NULL, vaddr, pte,
                   pfn_pte(phys >> PAGE_SHIFT, prot));
    else
        pte_clear(NULL, vaddr, pte);
    local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE);
}
```

This is the function that **writes directly into the hardware page table** and **flushes the TLB** so the CPU picks up the new mapping.

---

## 8. Complete Call Flow Diagram

```
setup_arch()
│
├─► early_fixmap_init()
│     │
│     └─► pmd_populate_kernel()
│           └─► Writes fixmap PMD entry in init_mm page tables
│               (Hardware page table now has a valid PMD for 0xFFC00000-0xFFF00000)
│
├─► early_ioremap_init()          ← THE FUNCTION WE ARE STUDYING
│     │
│     └─► early_ioremap_setup()   [mm/early_ioremap.c]
│           │
│           └─► For i = 0 to 6:
│                 WARN_ON_ONCE(prev_map[i])
│                 slot_virt[i] = __fix_to_virt(FIX_BTMAP_BEGIN - 32*i)
│               (Software slot table now initialized)
│
├─► parse_early_param()
│     │
│     └─► [Various early_param() handlers may call early_ioremap()]
│
│   [Later in setup_arch()...]
│
├─► early_ioremap_reset()
│     └─► after_paging_init = 1
│         (Flag: paging is now set up, use __late_set_fixmap)
│
└─► paging_init()
      └─► Full page tables created for entire physical memory
```

---

## 9. Memory Layout Diagram

```
VIRTUAL MEMORY (ARM32, at time early_ioremap_init() runs):

0xFFFFFFFF ┌──────────────────────────────────────────┐
           │  (unmapped)                              │
0xFFF00000 ├──────────────────────────────────────────┤ FIXADDR_TOP
           │  Permanent Fixmap entries:               │
           │  - FIX_EARLYCON_MEM_BASE                 │
           │  - FIX_KMAP_BEGIN..END                   │
           │  - FIX_TEXT_POKE0, FIX_TEXT_POKE1        │
           │  (These PTEs are SET by various init fn) │
           ├──────────────────────────────────────────┤ FIX_BTMAP_END (= __end_of_permanent)
           │  SLOT 6: 32 pages ← NOT YET MAPPED       │
           │  ...                                     │
           │  SLOT 1: 32 pages ← NOT YET MAPPED       │
           │  SLOT 0: 32 pages ← NOT YET MAPPED       │
0xFFC80000 ├──────────────────────────────────────────┤ FIX_BTMAP_BEGIN → FIXADDR_START
           │  (used by kmap, not early_ioremap)        │
0xFFC00000 ├──────────────────────────────────────────┤
           │  vmalloc / ioremap area                  │
           │  (NOT YET USABLE - paging_init pending)  │
0xF0000000 ├──────────────────────────────────────────┤
           │  Kernel direct-mapped RAM:               │
           │  0xC0000000 = phys 0x80000000            │
           │  Kernel code, data, stack all here       │
0xC0000000 ├──────────────────────────────────────────┤
           │  User space (currently empty/invalid)    │
0x00000000 └──────────────────────────────────────────┘

PHYSICAL MEMORY (example ARM32 board):
0xFFFFFFFF ┌──────────────────────────────────────────┐
           │  MMIO: GIC, UART, SPI, I2C, etc.         │
0x10000000 ├──────────────────────────────────────────┤
           │  RAM:                                    │
           │  0x80000000: Kernel image loaded here    │
           │  0x80100000: DTB (Device Tree Blob)      │
           │  0x81000000: Free RAM                    │
0x80000000 ├──────────────────────────────────────────┤
           │  Boot ROM (read-only)                    │
0x00000000 └──────────────────────────────────────────┘
```

---

## 10. Data Structures Deep Dive

### 10.1 The Three Arrays

```c
// mm/early_ioremap.c — all marked __initdata (freed after init)

// prev_map[slot]: Virtual address of the current mapping in each slot
// NULL means the slot is free
static void __iomem *prev_map[FIX_BTMAPS_SLOTS] __initdata;

// prev_size[slot]: Size of the current mapping (needed for unmapping)
static unsigned long prev_size[FIX_BTMAPS_SLOTS] __initdata;

// slot_virt[slot]: Base virtual address of each slot (PRE-COMPUTED at init time)
static unsigned long slot_virt[FIX_BTMAPS_SLOTS] __initdata;
```

### 10.2 The Fixed Addresses Enum

```c
// This is a compile-time constant enumeration
// Each value is an INDEX into the fixmap region
enum fixed_addresses {
    FIX_EARLYCON_MEM_BASE,      // = 0
    __end_of_permanent_fixed_addresses,  // = 1
    FIX_KMAP_BEGIN = 1,
    FIX_KMAP_END = 1 + (KM_MAX_IDX * NR_CPUS) - 1,
    FIX_TEXT_POKE0,
    FIX_TEXT_POKE1,
    __end_of_fixmap_region,     // = some value N

    // These SHARE the kmap region (kmap not used before paging_init):
    FIX_BTMAP_END = __end_of_permanent_fixed_addresses,  // = 1
    FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,  // = 1 + 224 - 1 = 224
};
```

### 10.3 Page Table Structures Used

```
ARM32 Two-Level Page Table:
PGD (Page Global Directory):
  - 4KB in size
  - 2048 entries × 2 bytes each = 4096 bytes
  - Each entry covers 2MB of virtual address space
  - Located at: init_mm.pgd (set up in head.S)

PTE (Page Table Entry):
  - 512 entries × 8 bytes each = 4096 bytes (ARM uses hardware + Linux PTEs)
  - Each entry covers 4KB (one page)
  - For fixmap: stored in bm_pte[] array (statically allocated, marked __page_aligned_bss)
```

---

## 11. Page Table Walk During early_ioremap

### 11.1 What happens when early_ioremap(0x10009000, 4096) is called?
(Example: Mapping the PL011 UART at physical 0x10009000)

```
STEP 1: Find free slot → slot = 0

STEP 2: size = 4096, phys_addr = 0x10009000
        offset = 0x10009000 & 0xFFF = 0  (already page-aligned)
        phys_addr = 0x10009000 & ~0xFFF = 0x10009000
        nrpages = 1

STEP 3: Install PTE
        idx = FIX_BTMAP_BEGIN - 32*0 = FIX_BTMAP_BEGIN

        __early_set_fixmap(FIX_BTMAP_BEGIN, 0x10009000, FIXMAP_PAGE_IO):
          vaddr = __fix_to_virt(FIX_BTMAP_BEGIN)
                = FIXADDR_TOP - FIX_BTMAP_BEGIN * PAGE_SIZE
                = e.g., 0xFFE00000  (example)

          pte = pte_offset_fixmap(pmd_off_k(vaddr), vaddr)
              → Points to the PTE entry in bm_pte[] for address 0xFFE00000

          set_pte_at(NULL, vaddr, pte, pfn_pte(0x10009, FIXMAP_PAGE_IO)):
          ┌─────────────────────────────────────────────────────┐
          │ HARDWARE PAGE TABLE ENTRY WRITTEN:                  │
          │ PTE at bm_pte[idx]:                                 │
          │   Physical Page Frame: 0x10009 (physical page)     │
          │   Attributes: non-cacheable, non-bufferable,        │
          │               device memory (FIXMAP_PAGE_IO)        │
          │               read-write, kernel-only              │
          └─────────────────────────────────────────────────────┘

          local_flush_tlb_kernel_range(0xFFE00000, 0xFFE01000):
          ┌─────────────────────────────────────────────────────┐
          │ CPU TLB FLUSH:                                      │
          │ Invalidates TLB entry for 0xFFE00000                │
          │ ARM32: MCR p15, 0, vaddr, c8, c7, 1  (TLBIMVAIS)   │
          │ This forces next access to do a hardware page walk  │
          └─────────────────────────────────────────────────────┘

STEP 4: prev_map[0] = 0xFFE00000 + 0 = 0xFFE00000
        return 0xFFE00000

RESULT: Virtual address 0xFFE00000 now maps to physical 0x10009000
        The kernel can now read/write UART registers!
```

### 11.2 Hardware Memory Access After Mapping

```
CPU executes: readl(uart_base + UART_DR)
              where uart_base = 0xFFE00000

CPU HARDWARE PROCESS:
1. CPU generates virtual address 0xFFE00000
2. MMU checks TLB → MISS (just flushed)
3. MMU hardware page table walker begins:
   a. Read TTB (Translation Table Base) register → points to init_mm.pgd
   b. Index into PGD with bits[31:21] of 0xFFE00000 = 0x7FF
   c. PGD[0x7FF] → PMD entry (valid, points to bm_pte table)
   d. Index into PMD with bits[20:12] of 0xFFE00000 = 0x000
   e. PTE[0x000] → Physical page 0x10009, attributes: device
4. MMU loads TLB entry: VA=0xFFE00000 → PA=0x10009000, device memory
5. Access proceeds to physical address 0x10009000
6. Bus transaction to hardware UART register completes
7. Data returned to CPU register
```

---

## 12. Hardware & Kernel Reaction Step by Step

### 12.1 Before early_ioremap_init() is called

| Component | State |
|-----------|-------|
| MMU | ON (enabled in head.S) |
| Kernel page tables | Minimal (kernel image mapped) |
| Fixmap PMD | EXISTS (set by early_fixmap_init) |
| Fixmap PTEs | ALL EMPTY (no physical mappings) |
| slot_virt[] | ALL ZERO (uninitialized) |
| prev_map[] | ALL NULL |

### 12.2 During early_ioremap_init()

```
early_ioremap_init()
  └─► early_ioremap_setup()
        Loop i = 0 to 6:
          WARN_ON_ONCE(prev_map[i])    → no-op (all NULL)
          slot_virt[i] = __fix_to_virt(FIX_BTMAP_BEGIN - 32*i)
          
          This is PURE COMPUTATION — no hardware access, no page table writes!
          Just pre-computing and storing virtual address constants.
```

**Key insight:** `early_ioremap_init()` itself does NOT write any page table entries. It only initializes the SOFTWARE data structures. The hardware page table writes happen later when `early_ioremap()` is actually called.

### 12.3 After early_ioremap_init() is called

| Component | State |
|-----------|-------|
| MMU | ON |
| Kernel page tables | Minimal (unchanged) |
| Fixmap PMD | EXISTS |
| Fixmap PTEs | ALL EMPTY (still) |
| slot_virt[] | POPULATED with virtual addresses |
| prev_map[] | ALL NULL (all slots free) |
| **Result** | **System can now call early_ioremap()** |

---

## 13. Lifecycle: From Boot to Paging Init

### 13.1 Complete Timeline

```
                    TIME →
────────────────────────────────────────────────────────────────────
boot.S              head.S              setup_arch()          rest of init
    │                   │                    │
    │ MMU disabled       │ MMU enabled        │ C code running
    │                    │                    │
    │                    │ Identity map       │ early_fixmap_init()
    │                    │ created            │   ↓
    │                    │                    │ early_ioremap_init()
    │                    │                    │   ↓
    │                    │                    │ early_ioremap() available
    │                    │                    │   ↓
    │                    │                    │ parse_early_param()
    │                    │                    │   (uses early_ioremap)
    │                    │                    │   ↓
    │                    │                    │ paging_init()
    │                    │                    │   ↓
    │                    │                    │ early_ioremap_reset()
    │                    │                    │   ↓
    │                    │                    │ ioremap() now available
    │                    │                    │   ↓
    │                    │                    │ free_initmem()
    │                    │                    │   __init sections freed
    │                    │                    │   early_ioremap no longer accessible
```

### 13.2 early_ioremap_reset()

```c
// mm/early_ioremap.c
static int after_paging_init __initdata;

void __init early_ioremap_reset(void)
{
    after_paging_init = 1;
}
```

After `paging_init()`, the flag `after_paging_init = 1`. This changes behavior:

```c
// In __early_ioremap():
while (nrpages > 0) {
    if (after_paging_init)
        __late_set_fixmap(idx, phys_addr, prot);  // uses regular set_fixmap
    else
        __early_set_fixmap(idx, phys_addr, prot); // uses boot-time set_fixmap
}
```

For ARM32, both are the same function (`__set_fixmap`). For ARM64, `__late_set_fixmap` can broadcast TLB invalidations to other CPUs (important for SMP systems).

### 13.3 Late Check: check_early_ioremap_leak()

```c
// mm/early_ioremap.c
static int __init check_early_ioremap_leak(void)
{
    int count = 0;
    int i;

    for (i = 0; i < FIX_BTMAPS_SLOTS; i++)
        if (prev_map[i])
            count++;

    if (WARN(count, "early ioremap leak of %d areas detected\n", count))
        return 1;
    return 0;
}
late_initcall(check_early_ioremap_leak);
```

This runs at `late_initcall` time and warns if any early ioremap mappings were never released. This is a **debug/sanity check** for kernel developers.

---

## 14. Interview Questions and Answers

### Q1: What is early_ioremap_init() and why is it needed?

**A:** `early_ioremap_init()` initializes a temporary I/O remapping mechanism that allows the Linux kernel to access physical memory-mapped I/O regions (hardware registers, device trees, ACPI tables) before the full virtual memory system is available. The Linux kernel boots with a minimal page table. The full `ioremap()` function requires the vmalloc area and slab allocator to be initialized, which happens much later in the boot sequence. `early_ioremap_init()` provides a workaround using pre-allocated fixmap virtual address slots that can be temporarily pointed at any physical address.

---

### Q2: What is the difference between ioremap() and early_ioremap()?

**A:**
| Feature | ioremap() | early_ioremap() |
|---------|-----------|-----------------|
| Available | After paging_init() | After early_ioremap_init() |
| Virtual addr | Dynamic (vmalloc area) | Fixed compile-time slots |
| Max mapping | Unlimited | 128KB per slot, 7 slots max |
| Persistent | Until iounmap() | Must be unmapped before init ends |
| Memory alloc | Allocates page table pages | Uses pre-allocated bm_pte[] |
| Thread safe | Yes | No (single-threaded boot) |

---

### Q3: What happens if you forget to call early_iounmap()?

**A:** The `check_early_ioremap_leak()` function (a `late_initcall`) will detect the leaked mapping and print a kernel warning. More practically: if you forget to unmap and then try to map more physical addresses than the 7 available slots allow, `__early_ioremap()` will fail to find a free slot and return NULL, potentially causing a NULL pointer dereference and kernel panic.

---

### Q4: Why does early_ioremap_init() come AFTER early_fixmap_init()?

**A:** `early_fixmap_init()` writes the actual hardware page table entry for the PMD that covers the fixmap virtual address region. Without this PMD entry, there is no page table structure to write PTEs into. If `early_ioremap_init()` (or more precisely `early_ioremap()`) were called first, then `__early_set_fixmap()` would try to modify PTEs in a PMD that doesn't exist, leading to a NULL pointer dereference in the page table walk or memory corruption.

---

### Q5: Why is this function marked __init?

**A:** The `__init` attribute places the function in the `.init.text` ELF section. Linux frees all `.init.*` sections after kernel initialization completes (via `free_initmem()`). This reclaims memory used by initialization code. `early_ioremap_init()` is only needed once during boot, so it qualifies for this optimization. Similarly, all the data arrays (`prev_map`, `prev_size`, `slot_virt`) are marked `__initdata` so they are also freed after init.

---

### Q6: Can early_ioremap() be called from interrupt context?

**A:** No. During early boot, interrupts are disabled, so there's no interrupt context. After the kernel runs normally, `early_ioremap()` has `WARN_ON(system_state >= SYSTEM_RUNNING)` which catches misuse. The entire early_ioremap mechanism is designed for single-threaded, non-preemptible early boot code only.

---

### Q7: How many bytes can early_ioremap() map at once?

**A:** Maximum of `NR_FIX_BTMAPS × PAGE_SIZE` per call:
- ARM32: `32 × 4096 = 131072 bytes = 128KB` per slot
- ARM64: `(256KB / 4KB) × 4096 = 256KB` per slot

And there are 7 slots (FIX_BTMAPS_SLOTS = 7), so the maximum total concurrent mapping is:
- ARM32: `7 × 128KB = 896KB`
- ARM64: `7 × 256KB = 1792KB`

---

### Q8: What is __initdata and why are the arrays marked with it?

**A:** `__initdata` places variables in the `.init.data` section. Just like `__init` code, these variables are freed after kernel initialization. The arrays `prev_map`, `prev_size`, and `slot_virt` only need to exist during early boot. Marking them `__initdata` saves RAM after the boot sequence completes — typically several kilobytes of kernel memory is reclaimed.

---

*Document created: 2026-05-08*
*Kernel Version: Linux (ARM/ARM64 architecture)*
*Source files referenced:*
- *arch/arm/kernel/setup.c*
- *arch/arm/mm/ioremap.c*
- *arch/arm/mm/mmu.c*
- *arch/arm/include/asm/fixmap.h*
- *mm/early_ioremap.c*
- *include/asm-generic/early_ioremap.h*
- *include/asm-generic/fixmap.h*
