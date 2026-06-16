# Layer 6 — Page Tables & `paging_init()`

[← Layer 5: Memblock Early Allocator](05_Memblock_Early_Allocator.md) | **Layer 6** | [Layer 7: Zones, Nodes & Page Init →](07_Zones_Nodes_and_Page_Init.md)

---

## What This Layer Covers

The kernel knows about all physical RAM (memblock) and can allocate from it. But there's
still no way to access arbitrary physical memory through virtual addresses — only the
kernel image and the fixmap are mapped. This layer explains how `paging_init()` creates
the **linear map** (also called "direct map") — a contiguous virtual mapping of ALL
physical RAM in `swapper_pg_dir`. After this layer, any physical address can be accessed
via its corresponding virtual address using `__va()`.

---

## 1. Why the Linear Map Matters

```
┌──────────────────────────────────────────────────────────────────┐
│                    THE PROBLEM                                    │
│                                                                  │
│  The kernel image is mapped at KIMAGE_VADDR (e.g., 0xFFFF_8000) │
│  but ALL OTHER physical RAM has no virtual mapping.              │
│                                                                  │
│  The kernel needs to:                                            │
│    • Allocate struct page arrays for every page of RAM           │
│    • Access page table pages allocated by memblock               │
│    • Read/write any physical memory (DMA buffers, user pages)    │
│                                                                  │
│  Solution: map ALL physical RAM at a fixed virtual offset.       │
│                                                                  │
│  This is the LINEAR MAP:                                         │
│    For any physical address PA:                                  │
│      Virtual Address = PA - PHYS_OFFSET + PAGE_OFFSET            │
│                                                                  │
│  It's "linear" because the mapping is a simple offset —          │
│  physical addresses appear in the same order as virtual ones.    │
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. `paging_init()` — The Master Function

**Source**: `arch/arm64/mm/mmu.c`

```
paging_init()
    │
    ├──► map_mem(swapper_pg_dir)         [Step 1] ★ Create the linear map ★
    │       Map ALL physical RAM into the kernel's VA space
    │       at PAGE_OFFSET.
    │
    ├──► memblock_allow_resize()         [Step 2]
    │       Until now, memblock arrays could not be resized
    │       (because resizing requires allocating from the linear
    │        map, which didn't exist). Now resizing is allowed.
    │
    ├──► create_idmap()                  [Step 3]
    │       Create the runtime identity map in idmap_pg_dir.
    │       Only maps the .idmap.text section (small).
    │
    └──► declare_kernel_vmas()           [Step 4]
            Register kernel VA regions with the vmalloc subsystem.
```

---

## 3. `map_mem()` — Creating the Linear Map

**Source**: `arch/arm64/mm/mmu.c`

This is the most important function in this layer. It iterates over every memblock memory
region and creates page table entries mapping physical RAM → virtual addresses.

### 3.1 The Algorithm

```
map_mem(pgdp = swapper_pg_dir)
    │
    ├──► 1. Temporarily mark kernel text as NOMAP
    │       memblock_mark_nomap(_text, __init_begin - _text)
    │       Why? The kernel image is already mapped at KIMAGE_VADDR.
    │       We DON'T want the linear map to create an executable
    │       alias of the kernel text (security: W^X enforcement).
    │       The kernel text will be linear-mapped as NON-executable.
    │
    ├──► 2. Iterate all memblock memory regions
    │       for_each_mem_range(i, &start, &end):
    │       │
    │       │   For each region that IS NOT marked NOMAP:
    │       │
    │       └──► __map_memblock(pgdp, start, end, prot, flags)
    │            │
    │            └── Compute virtual address:
    │                   virt = __phys_to_virt(start)
    │                        = (start - PHYS_OFFSET) + PAGE_OFFSET
    │
    │                Call __create_pgd_mapping(pgdp, start, virt,
    │                                          size, prot, ...)
    │                    Create page table entries in swapper_pg_dir
    │                    mapping PA [start, end) → VA [virt, virt+size)
    │
    ├──► 3. Re-map kernel text (linear alias) as non-executable
    │       The kernel text was temporarily NOMAP'd. Now map it
    │       back, but as PAGE_KERNEL (RW, NX) — NOT executable.
    │       This prevents code execution from the linear map alias.
    │
    └──► 4. Unmark kernel text as NOMAP
            memblock_clear_nomap(_text, __init_begin - _text)
```

### 3.2 Block Mappings — Efficiency

The kernel uses **block mappings** wherever possible for the linear map. Instead of
creating 4KB page table entries for every page, it creates larger mappings:

```
Page Table Level    Block Size    When Used
════════════════    ══════════    ═════════
Level 1 (PUD)       1 GB         If region is 1 GB aligned and >= 1 GB
Level 2 (PMD)       2 MB         If region is 2 MB aligned and >= 2 MB
Level 3 (PTE)       4 KB         For unaligned edges, or when forced

Example: Mapping 2 GB of RAM starting at 0x4000_0000:

  VA Range                          Mapping Type
  ────────                          ────────────
  0xFFFF_0000_0000_0000 + 0 GB     1 GB block (PUD entry)
  0xFFFF_0000_0000_0000 + 1 GB     1 GB block (PUD entry)

  Only 2 page table entries needed instead of 524,288 PTE entries!

  If 1 GB blocks aren't possible (alignment):
  0xFFFF_0000_0000_0000 + 0 MB     2 MB block (PMD entry)
  0xFFFF_0000_0000_0000 + 2 MB     2 MB block (PMD entry)
  ...
  = 1024 entries instead of 524,288
```

**Contiguous entries**: ARM64 also supports a **contiguous hint** in page table entries.
When 16 adjacent entries map a contiguous physical range, the TLB can coalesce them into
a single entry:
- 16 × PMD (2 MB) = **32 MB contiguous** block
- 16 × PTE (4 KB) = **64 KB contiguous** block

### 3.3 When Block Mappings Are NOT Used

Certain configurations force PTE-level (4KB) mappings:

```
Force PTE mapping when:
  • CONFIG_DEBUG_PAGEALLOC — needs per-page map/unmap for debugging
  • CONFIG_KFENCE — kernel fence requires PTE granularity
  • Arm CCA / Realms — needs PTE-level permission control
  • KFENCE pool region — always mapped at PTE level
```

---

## 4. The Page Table Creation Hierarchy

**Source**: `arch/arm64/mm/mmu.c`

When `map_mem()` calls `__create_pgd_mapping()`, it triggers a cascade of functions that
walk/create the multi-level page table:

```
__create_pgd_mapping(pgdp, phys, virt, size, prot, alloc, flags)
    │
    │   Walk/create PGD entry for the virtual address
    │
    └──► alloc_init_p4d(pgdp, addr, end, phys, prot, alloc, flags)
         │
         │   Walk/create P4D entry (only used with 5-level paging)
         │
         └──► alloc_init_pud(p4dp, addr, end, phys, prot, alloc, flags)
              │
              │   Can the PUD entry be a 1 GB block?
              │   ├─ YES (aligned, no force_pte, no NOBLOCK flag):
              │   │     Write PUD block entry → DONE for this range
              │   │
              │   └─ NO: Allocate a PMD table page → recurse
              │
              └──► alloc_init_cont_pmd(pudp, addr, end, phys, prot, ...)
                   │
                   │   Handle contiguous PMD entries (16 × 2MB = 32MB)
                   │
                   └──► init_pmd(pudp, addr, end, phys, prot, ...)
                        │
                        │   Can the PMD entry be a 2 MB block?
                        │   ├─ YES: Write PMD block entry → DONE
                        │   │
                        │   └─ NO: Allocate a PTE table page → recurse
                        │
                        └──► alloc_init_cont_pte(pmdp, addr, end, phys, ...)
                             │
                             │   Handle contiguous PTE entries (16 × 4KB = 64KB)
                             │
                             └──► init_pte(pmdp, addr, end, phys, prot)
                                      Write PTE entries (4 KB pages)
                                      This is the bottom level.
```

### 4.1 Page Table Page Allocation

Each time a new level of page table is needed, a page must be allocated for it.
During boot, this allocation comes from **memblock**:

```
pgtable_alloc(size)     [during boot]
    │
    └──► early_pgtable_alloc(size)
         │
         ├── phys = memblock_phys_alloc(size, size)
         │       Allocate a physical page from memblock.
         │
         ├── ptr = __va(phys)
         │       Convert to virtual address (linear map).
         │       NOTE: This works because we're building the linear
         │       map incrementally — previously mapped regions are
         │       accessible.
         │
         └── memset(ptr, 0, size)
                Zero the page table page.
```

---

## 5. `create_idmap()` — The Runtime Identity Map

**Source**: `arch/arm64/mm/mmu.c`

After creating the linear map, `paging_init()` creates a compact **runtime identity map**
in `idmap_pg_dir`. This replaces the boot-time identity map (`init_idmap_pg_dir`) which
mapped the entire kernel image.

```
create_idmap()
    │
    ├──► Only maps: __idmap_text_start → __idmap_text_end
    │       This is the .idmap.text section — a small set of functions
    │       that need to execute with VA == PA.
    │       (e.g., cpu_switch_mm, cpu_resume, etc.)
    │
    ├──► Protection: PAGE_KERNEL_ROX (Read-Only + Executable)
    │
    └──► Uses idmap_pg_dir (1 page only — very compact)

Why keep an identity map at all?
    Some operations require temporarily switching page tables
    (e.g., suspend/resume, CPU hotplug, kexec). During the switch,
    the CPU must briefly execute from identity-mapped addresses.
```

---

## 6. `declare_kernel_vmas()` — Register Kernel VA Areas

After the linear map and identity map are created, the kernel registers its VA regions
with the vmalloc subsystem so that `vmalloc()` knows which addresses are already taken:

```
declare_kernel_vmas()
    │
    ├──► Register kernel image VMA
    ├──► Register linear map region
    └──► Register other reserved VA ranges
```

---

## 7. The Result: Complete Page Table State

After `paging_init()` completes:

```
swapper_pg_dir (TTBR1)
═══════════════════════

Virtual Address Range                    Physical Mapping              Level
──────────────────────                   ─────────────────              ─────
PAGE_OFFSET + 0                          DRAM Bank 1 (0x4000_0000)    PUD blocks (1GB)
  to PAGE_OFFSET + 2GB                                                or PMD blocks (2MB)

PAGE_OFFSET + gap_offset                 DRAM Bank 2 (0x1_0000_0000) PUD/PMD blocks
  to PAGE_OFFSET + gap_offset + 4GB

KIMAGE_VADDR + kaslr_offset             Kernel image (0x4080_0000)   PMD/PTE entries
  to KIMAGE_VADDR + kaslr_offset + imgsize  (separate from linear map)

FIXADDR_TOT_START                        Various (FDT, early IO)     PTE entries
  to FIXADDR_TOP                           (via fixmap)


idmap_pg_dir (loaded into TTBR0 only when needed)
═════════════════════════════════════════════════

Virtual Address Range                    Physical Mapping              Level
──────────────────────                   ─────────────────              ─────
__idmap_text_start                       Same physical address         PMD/PTE
  to __idmap_text_end                    (VA == PA)
```

### 7.1 Visual: The Linear Map

```
Physical Memory:                     Kernel Virtual Address Space:

                                     PAGE_END
                                     0xFFFF_8000_0000_0000
                                         ┌─────────────┐
                                         │  (kernel img,│
                                         │   vmalloc,   │
                                         │   etc.)      │
                                         ├─────────────┤
0x1_4000_0000 ┌──────────┐              │             │
              │ DRAM      │◄── linear ──│ Bank 2      │
              │ Bank 2    │    map       │ (4 GB)      │
0x1_0000_0000 └──────────┘              ├─────────────┤
                                         │ (hole)      │
              (physical hole)            │ unmapped    │
                                         ├─────────────┤
0xC000_0000   ┌──────────┐              │             │
              │ DRAM      │◄── linear ──│ Bank 1      │
              │ Bank 1    │    map       │ (2 GB)      │
0x4000_0000   └──────────┘              │             │
                                         └─────────────┘
                                     PAGE_OFFSET
                                     0xFFFF_0000_0000_0000

     Physical holes (no RAM) are NOT mapped in the linear map.
     Only actual memblock.memory regions get mapped.
```

---

## 8. Page Table Entry Format (ARM64 Detail)

Each page table entry (PTE) on ARM64 is 64 bits:

```
PTE / Block Entry Format (64-bit):
═══════════════════════════════════

Bit(s)   Field              Meaning
──────   ─────              ───────
[0]      Valid              1 = entry is valid, 0 = fault
[1]      Table/Block        1 = table pointer (or page), 0 = block

[4:2]    AttrIndx           Index into MAIR_EL1 (memory type)
                            0 = MT_NORMAL (cacheable RAM)
                            3 = MT_DEVICE_nGnRnE

[6]      AP[1]              0 = read-write, 1 = read-only
[7]      AP[2]              0 = EL1 only, 1 = EL0 accessible

[8:9]    SH                 Shareability (10 = Outer, 11 = Inner)

[10]     AF                 Access Flag (1 = accessed)

[11]     nG                 not-Global (1 = ASID-tagged TLB entry)

[47:12]  Output Address     Physical address of page/block
                            (bits [47:12] for 4KB pages)

[52]     Contiguous          Hint: part of a contiguous set

[53]     PXN                Privileged Execute-Never
[54]     UXN/XN             User/Unprivileged Execute-Never

[55]     Software bit       Used by Linux (e.g., dirty, young)
```

### 8.1 Protection Macros Used During Boot

| Macro | AttrIndx | AP | XN | Meaning |
|-------|----------|----|----|---------|
| `PAGE_KERNEL` | MT_NORMAL | RW | XN | Normal RAM, read-write, no-execute |
| `PAGE_KERNEL_RO` | MT_NORMAL | RO | XN | Normal RAM, read-only, no-execute |
| `PAGE_KERNEL_ROX` | MT_NORMAL | RO | — | Normal RAM, read-only, executable |
| `PAGE_KERNEL_EXEC` | MT_NORMAL | RW | — | Normal RAM, read-write, executable |
| `PROT_DEVICE_nGnRnE` | MT_DEVICE_nGnRnE | RW | XN | Device MMIO |

---

## 9. State After Layer 6

```
┌────────────────────────────────────────────────────────────────┐
│              STATE AFTER paging_init()                          │
│                                                                │
│  Page Tables (swapper_pg_dir):                                 │
│    ✓ Kernel image mapped at KIMAGE_VADDR                       │
│    ✓ LINEAR MAP: ALL physical RAM mapped at PAGE_OFFSET ★      │
│    ✓ Fixmap active (FDT, early IO)                             │
│    ✓ Kernel text linear alias is non-executable (security)     │
│                                                                │
│  Identity Map (idmap_pg_dir):                                  │
│    ✓ Compact runtime idmap (.idmap.text only)                  │
│                                                                │
│  Virtual Address Translation:                                  │
│    ✓ __va(PA) works — convert any physical addr to virtual     │
│    ✓ __pa(VA) works — convert linear map VA to physical        │
│    ✓ __pa_symbol(VA) works — convert kernel image VA to phys   │
│                                                                │
│  Memory Allocator:                                             │
│    ✓ memblock works and can use linear map for __va()           │
│    ✗ Buddy allocator NOT ready (no struct page, no zones)      │
│    ✗ Slab NOT ready                                            │
│                                                                │
│  What Comes Next:                                              │
│    → bootmem_init (Layer 7): set up NUMA nodes, zones          │
│    → free_area_init (Layer 7): allocate struct page arrays,    │
│      initialize zone free lists                                │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 6

| Step | Function | What It Did |
|------|----------|-------------|
| 1 | `map_mem()` | Created linear map: ALL RAM mapped at PAGE_OFFSET with block mappings (2MB/1GB) |
| 2 | `memblock_allow_resize()` | Enabled memblock array growth (safe now with linear map) |
| 3 | `create_idmap()` | Created compact runtime identity map in `idmap_pg_dir` |
| 4 | `declare_kernel_vmas()` | Registered kernel VA regions with vmalloc subsystem |

**The linear map is the backbone of Linux memory management**. Almost all kernel memory
access goes through it. With the linear map in place, the kernel can now access any
physical RAM via virtual addresses, which is essential for the next steps: creating
zone structures, `struct page` arrays, and initializing the buddy allocator.

---

[← Layer 5: Memblock Early Allocator](05_Memblock_Early_Allocator.md) | **Layer 6** | [Layer 7: Zones, Nodes & Page Init →](07_Zones_Nodes_and_Page_Init.md)
