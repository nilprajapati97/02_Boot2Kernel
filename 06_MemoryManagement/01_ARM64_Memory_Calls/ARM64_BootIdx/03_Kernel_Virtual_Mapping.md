# Layer 3 — Kernel Virtual Mapping & VA Layout

[← Layer 2: Early Assembly Boot](02_Early_Assembly_Boot.md) | **Layer 3** | [Layer 4: Device Tree & Memory Discovery →](04_Device_Tree_and_Memory_Discovery.md)

---

## What This Layer Covers

With the MMU on and the CPU running from the identity map (VA == PA), the kernel must
now create its proper virtual address mapping. This layer explains how the kernel maps
itself at a high virtual address, optionally applies KASLR randomization, and transitions
from identity-mapped physical addresses to the kernel's virtual address space. It also
details the complete ARM64 virtual memory layout.

---

## 1. `early_map_kernel` — Mapping the Kernel at Its Virtual Address

**Source**: `arch/arm64/kernel/pi/map_kernel.c`

After `__enable_mmu` returns in `__primary_switch`, the CPU executes from the identity
map (TTBR0). The next call is to `early_map_kernel()`, which runs as position-independent
code (all function calls via `__pi_` prefix, all addresses computed relative to PC).

### 1.1 What `early_map_kernel` Does (Step by Step)

```
early_map_kernel(x0 = boot_status, x1 = FDT pointer)
    │
    ├──► 1. Clear BSS section to zero
    │       Also clear init_pg_dir and init_idmap_pg_dir
    │       (start fresh for the real kernel mapping)
    │
    ├──► 2. Map the FDT into the identity map
    │       map_fdt() — creates a temporary mapping of the Device Tree
    │       in init_idmap_pg_dir so early parsing can read it.
    │
    ├──► 3. Parse command line for CPU feature overrides
    │       init_feature_override() — reads early command-line params
    │       that affect VA bits, page size, etc.
    │
    ├──► 4. Determine VA space size at runtime
    │       Check if 52-bit VA is supported (LVA extension).
    │       If not, fall back to VA_BITS_MIN (e.g., 48-bit).
    │       This affects the entire kernel VA layout.
    │
    ├──► 5. KASLR — Kernel Address Space Layout Randomization
    │       kaslr_early_init() reads a random seed from:
    │         - /chosen/kaslr-seed in the DTB, or
    │         - EFI RNG protocol, or
    │         - Hardware RNG (RNDR instruction)
    │       Computes a random kaslr_offset to slide the kernel.
    │
    ├──► 6. Remap identity map for LPA2 (if 52-bit PA)
    │       remap_idmap_for_lpa2() — adjusts the identity map if
    │       using 52-bit physical addresses with LPA2 extension.
    │
    ├──► 7. map_kernel() ★ THE MAIN EVENT ★
    │       Creates the kernel's virtual mapping in init_pg_dir.
    │       (Explained in detail below)
    │
    └──► return → __primary_switch continues
```

### 1.2 `map_kernel()` — Mapping the Kernel Image Segments

`map_kernel()` creates six separate mappings in `init_pg_dir`, each with appropriate
permissions:

```
Kernel Image Segments Mapped into init_pg_dir
══════════════════════════════════════════════

Virtual Address (at KIMAGE_VADDR + kaslr_offset):

  High ┌─────────────────────────────────┐
       │  Segment 6: .data + .bss        │  PAGE_KERNEL (RW, NX)
       │  Read-write kernel data          │
       ├─────────────────────────────────┤
       │  Segment 5: .init.data           │  PAGE_KERNEL (RW, NX)
       │  Init-only data (freed later)    │
       ├─────────────────────────────────┤
       │  Segment 4: .init.text           │  PAGE_KERNEL_ROX (RO, X)
       │  Init-only code (freed later)    │
       ├─────────────────────────────────┤
       │  Segment 3: .rodata              │  PAGE_KERNEL (RO, NX)
       │  Read-only data                  │
       ├─────────────────────────────────┤
       │  Segment 2: .text (_stext)       │  PAGE_KERNEL_ROX (RO, X)
       │  Kernel executable code          │
       ├─────────────────────────────────┤
       │  Segment 1: pre-text             │  PAGE_KERNEL (RW, NX)
       │  Image header, metadata          │
  Low  └─────────────────────────────────┘
         ↑
         KIMAGE_VADDR + kaslr_offset
```

**Protection flags**:
- `PAGE_KERNEL_ROX` = Read-Only + Executable — for code sections
- `PAGE_KERNEL` = Read-Write + No-Execute — for data sections
- Combined with `WXN` bit in SCTLR: any writable page is automatically non-executable

### 1.3 The TTBR1 Switch

After `map_kernel()` creates the virtual mapping in `init_pg_dir`:

```
Step A:  idmap_cpu_replace_ttbr1(init_pg_dir)
         ├─ Load reserved_pg_dir into TTBR1 (safe empty table)
         ├─ TLB invalidate all EL1 entries
         ├─ Load init_pg_dir into TTBR1 ★
         └─ Barrier + ISB

Step B:  Copy init_pg_dir root entries → swapper_pg_dir

Step C:  idmap_cpu_replace_ttbr1(swapper_pg_dir)
         ├─ Switch TTBR1 to swapper_pg_dir ★
         └─ swapper_pg_dir is now the PERMANENT kernel page table root

Result:
  TTBR0 = init_idmap_pg_dir (identity map, still present)
  TTBR1 = swapper_pg_dir ★ (kernel at high virtual address)
```

### 1.4 Relocations and KASLR

If `CONFIG_RANDOMIZE_BASE` is enabled and a KASLR offset was computed:

```
After map_kernel():
    ├─ Apply ELF relocations (RELA entries) to adjust all
    │  absolute addresses in the kernel for the new random VA.
    │
    └─ Patch Shadow Call Stack (SCS) offsets if enabled.
```

This is done while still executing from the identity map (TTBR0), because the relocation
code itself is position-independent.

---

## 2. `__primary_switched` — Entering the Virtual World

**Source**: `arch/arm64/kernel/head.S`

After `early_map_kernel()` returns to `__primary_switch`:

```
__primary_switch (continuing):
    │
    ├──► ldr  x8, =__primary_switched   // Virtual address!
    ├──► adrp x0, KERNEL_START           // Physical base of kernel
    └──► br   x8                         // ★ JUMP TO VIRTUAL ADDRESS ★
```

This `br x8` is the pivotal moment: the CPU jumps from executing at a physical address
(via TTBR0 identity map) to executing at a high virtual address (via TTBR1 kernel map).

### 2.1 What `__primary_switched` Does

```
__primary_switched:
    │
    ├──► Set up init_task stack
    │       SP = init_task + THREAD_SIZE (top of kernel stack)
    │       This is the boot CPU's initial kernel stack.
    │
    ├──► Set per-CPU offset to 0 (boot CPU)
    │
    ├──► Save FDT pointer
    │       __fdt_pointer = x21 (FDT physical address)
    │
    ├──► Compute kimage_voffset
    │       kimage_voffset = _text(virtual) - _text(physical)
    │       This offset converts between kernel VA and PA.
    │       Used by __pa_symbol() and __va() for kernel image addresses.
    │
    ├──► set_cpu_boot_mode_flag (EL1 or EL2)
    │
    ├──► kasan_early_init (if CONFIG_KASAN)
    │       Set up KASAN shadow memory for early use.
    │
    ├──► finalise_el2
    │       If VHE is available, prefer running at EL2 with VHE.
    │
    └──► bl start_kernel        ★ NEVER RETURNS ★
            The C world begins.
            This is where Layer 4 picks up.
```

---

## 3. ARM64 Virtual Memory Layout

With `swapper_pg_dir` active in TTBR1, here is the complete kernel virtual address layout
for 48-bit VA with 4KB pages:

### 3.1 Full VA Space Diagram

```
ARM64 Virtual Address Space (48-bit VA, 4KB pages)
═══════════════════════════════════════════════════

0xFFFF_FFFF_FFFF_FFFF ┌──────────────────────────────┐
                      │                              │
0xFFFF_FFFF_FF80_0000 │  FIXADDR (fixed mappings)    │  8 MB
                      │  Compile-time VA for temp    │
                      │  mappings: FDT, early IO     │
                      ├──────────────────────────────┤
0xFFFF_FFFF_FE80_0000 │  PCI I/O Space               │  16 MB
                      │  PCI device I/O ports         │
                      ├──────────────────────────────┤
                      │  Guard hole (8 MB)            │
                      ├──────────────────────────────┤
0xFFFF_FFFF_C000_0000 │  VMEMMAP                      │  ~ 1 GB
                      │  Virtual mem_map array        │
                      │  (struct page for all RAM)    │
                      ├──────────────────────────────┤
                      │  Guard hole (8 MB)            │
                      ├──────────────────────────────┤
                      │                              │
                      │  VMALLOC Space                │  ~ 126 GB
                      │  vmalloc(), ioremap(),        │
                      │  vmap() allocations            │
                      │                              │
                      ├──────────────────────────────┤
0xFFFF_8000_8000_0000 │  MODULES (2 GB)               │
                      │  Loadable kernel modules      │
                      ├──────────────────────────────┤
                      │  KIMAGE_VADDR                 │
                      │  ┌── Kernel Image ──┐         │
                      │  │ .text (code)     │         │
                      │  │ .rodata          │         │
                      │  │ .init.text/.data │         │
                      │  │ .data / .bss     │         │
                      │  │ swapper_pg_dir   │         │
                      │  └──────────────────┘         │
                      ├──────────────────────────────┤
0xFFFF_8000_0000_0000 │  PAGE_END                     │
                      │                              │  ← TTBR1 space start
                      │                              │
                      │  Linear Map (Direct Map)     │  128 TB (48-bit)
                      │                              │
                      │  ALL physical RAM mapped      │
                      │  contiguously here.            │
                      │  __va(pa) = pa - PHYS_OFFSET  │
                      │            + PAGE_OFFSET       │
                      │                              │
0xFFFF_0000_0000_0000 │  PAGE_OFFSET                  │
                      └──────────────────────────────┘

  ─────── Unmapped Hole (fault on access) ──────────

0x0000_FFFF_FFFF_FFFF ┌──────────────────────────────┐
                      │                              │
                      │  User Space (per process)    │  256 TB
                      │  TTBR0 — each process has    │
                      │  its own page tables here    │
                      │                              │
0x0000_0000_0000_0000 └──────────────────────────────┘
```

### 3.2 Key Regions Explained

| Region | Start | Size | Purpose |
|--------|-------|------|---------|
| **Linear Map** | `PAGE_OFFSET` | 128 TB | 1:1 mapping of ALL physical RAM. `__va()` and `__pa()` convert between PA and linear map VA. Created by `paging_init()` (Layer 6). |
| **Kernel Image** | `KIMAGE_VADDR` | ~tens of MB | The kernel binary. Mapped separately from the linear map with proper RO/RW/X permissions. May be KASLR-randomized. |
| **Modules** | `MODULES_VADDR` | 2 GB | Loadable kernel modules. Within branch range of kernel image. |
| **VMALLOC** | `VMALLOC_START` | ~126 GB | Virtually contiguous allocations (`vmalloc`), I/O mappings (`ioremap`). Pages may be physically discontiguous. |
| **VMEMMAP** | `VMEMMAP_START` | variable | Virtual memory map — the `struct page` array. One `struct page` per physical page frame. |
| **PCI I/O** | near top | 16 MB | Legacy PCI I/O port space mapped to virtual addresses. |
| **Fixmap** | near top | 8 MB | Compile-time-constant virtual addresses for temporary boot-time mappings (FDT, early console, etc.). |

### 3.3 The Linear Map (Direct Map)

The **linear map** is the most important memory region. It maps ALL physical RAM at a
fixed offset from `PAGE_OFFSET`:

```
Physical:    0x0000_0000_4000_0000  (DRAM base, e.g., 1 GB)
                      ↓
Virtual:     PAGE_OFFSET + (PA - PHYS_OFFSET)
             = 0xFFFF_0000_0000_0000 + (0x4000_0000 - 0x4000_0000)
             = 0xFFFF_0000_0000_0000

Where:
    PHYS_OFFSET = memstart_addr = base of first DRAM bank
    PAGE_OFFSET = -(1UL << VA_BITS) = 0xFFFF_0000_0000_0000 (for 48-bit)
```

**The linear map is NOT created yet at this point in boot**. It will be created by
`paging_init()` → `map_mem()` in Layer 6. Right now, only the kernel image itself is
mapped (at KIMAGE_VADDR via `swapper_pg_dir`).

---

## 4. Physical ↔ Virtual Address Translation

The kernel uses several macros to convert between physical and virtual addresses. There
are two domains:

### 4.1 Kernel Image Addresses

For addresses within the kernel image (`.text`, `.data`, etc.):

```
Physical → Virtual:   (not commonly needed)
Virtual  → Physical:  __pa_symbol(vaddr) = vaddr - kimage_voffset

Where:
    kimage_voffset = _text(virtual) - _text(physical)
                   = KIMAGE_VADDR + kaslr_offset - kernel_physical_base
```

### 4.2 Linear Map Addresses

For addresses in the linear map (most of kernel memory):

```
Physical → Virtual:   __va(pa)  = (pa - PHYS_OFFSET) | PAGE_OFFSET
                                = (pa - memstart_addr) + PAGE_OFFSET

Virtual  → Physical:  __pa(va)  = (va - PAGE_OFFSET) + PHYS_OFFSET
                      __lm_to_phys(va) = (va - PAGE_OFFSET) + PHYS_OFFSET
```

### 4.3 Relationship Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                                                              │
│   Physical Address 0x4200_0000                               │
│        │                                                     │
│        ├──── Kernel Image mapping ──► 0xFFFF_8000_1020_0000  │
│        │     (via kimage_voffset)      (KIMAGE_VADDR range)  │
│        │                                                     │
│        └──── Linear Map ─────────► 0xFFFF_0000_0200_0000     │
│              (via PHYS_OFFSET)       (PAGE_OFFSET range)     │
│                                                              │
│   The SAME physical page can be accessed through TWO         │
│   different virtual addresses! The kernel image has both     │
│   its own mapping AND a linear map alias.                    │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 5. State After Layer 3

```
┌────────────────────────────────────────────────────────────────┐
│              CPU STATE AFTER __primary_switched                 │
│                                                                │
│  Execution:    Running from TTBR1 (kernel virtual address) ✓   │
│                Identity map still in TTBR0 (removed later)      │
│                                                                │
│  TTBR0:        init_idmap_pg_dir (identity map — still active) │
│  TTBR1:        swapper_pg_dir ★ (kernel image mapped)          │
│                                                                │
│  Mapped regions:                                               │
│    ✓ Kernel image at KIMAGE_VADDR (text/rodata/data/bss)       │
│    ✓ FDT temporarily in identity map                           │
│    ✗ Linear map of all RAM (NOT YET — Layer 6)                 │
│    ✗ VMALLOC space (NOT YET — Layer 9)                         │
│    ✗ VMEMMAP (NOT YET — Layer 6/7)                             │
│                                                                │
│  Key variables set:                                            │
│    __fdt_pointer     = physical address of DTB                 │
│    kimage_voffset    = virtual - physical offset for kernel    │
│    init_task         = boot CPU's task_struct (stack ready)    │
│                                                                │
│  Memory allocator:   NONE (still no way to allocate memory)    │
│  RAM knowledge:      NONE (haven't parsed DTB yet)             │
│                                                                │
│  Next: start_kernel() → setup_arch() → parse DTB for memory   │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 3

| Step | What Happened |
|------|---------------|
| BSS cleared | Zero-initialized sections ready for use |
| FDT mapped | Device tree accessible for early parsing |
| KASLR applied | Kernel virtual address randomized (if enabled) |
| Kernel mapped | 6 segments at KIMAGE_VADDR with proper RO/RW/X permissions |
| TTBR1 switched | `swapper_pg_dir` is now the permanent kernel page table root |
| Relocations applied | All absolute addresses adjusted for KASLR offset |
| `kimage_voffset` computed | PA↔VA conversion for kernel image addresses works |
| `start_kernel()` called | The C entry point — Layer 4 begins |

**Critical understanding**: At this point, the kernel can execute its own code (mapped at
KIMAGE_VADDR), but it CANNOT access arbitrary physical RAM. It doesn't know how much RAM
exists, and there is no linear map yet. The kernel must parse the Device Tree to discover
memory — that's Layer 4.

---

[← Layer 2: Early Assembly Boot](02_Early_Assembly_Boot.md) | **Layer 3** | [Layer 4: Device Tree & Memory Discovery →](04_Device_Tree_and_Memory_Discovery.md)
