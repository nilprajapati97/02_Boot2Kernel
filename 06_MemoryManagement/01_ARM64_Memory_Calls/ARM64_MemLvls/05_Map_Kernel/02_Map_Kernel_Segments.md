# `map_kernel()` — Segment Mapping & Two-Pass

**Source:** `arch/arm64/kernel/pi/map_kernel.c` lines 41–152

## Purpose

`map_kernel()` creates page table entries in `init_pg_dir` that map each kernel segment (text, rodata, init, data) at its virtual address. It uses a two-pass approach when relocations are needed, and finishes by copying the root PGD to `swapper_pg_dir`.

## Function Signature

```c
static void __init map_kernel(u64 kaslr_offset, u64 va_offset, int root_level)
```

- `kaslr_offset`: Random displacement for KASLR
- `va_offset`: `virtual_base - physical_base` (the mapping offset)
- `root_level`: Starting page table level (0 for 4-level, 1 for 3-level)

## Page Table Allocation

```c
phys_addr_t pgdp = (phys_addr_t)init_pg_dir + PAGE_SIZE;
```

`init_pg_dir` is a statically allocated region in the kernel image. The first page is the PGD (root), and subsequent pages are used as PUD/PMD/PTE tables. `pgdp` is a "bump allocator" — each call to `map_segment` advances it to the next free page.

## Segment Mapping (6 segments)

```c
// 1. .head.text: contains non-executable boot code
map_segment(init_pg_dir, &pgdp, va_offset, _text, _stext,
            data_prot, false, root_level);

// 2. .text: executable kernel code
map_segment(init_pg_dir, &pgdp, va_offset, _stext, _etext,
            prot, !twopass, root_level);

// 3. .rodata: read-only data
map_segment(init_pg_dir, &pgdp, va_offset, __start_rodata,
            __inittext_begin, data_prot, false, root_level);

// 4. .init.text: initialization code (freed after boot)
map_segment(init_pg_dir, &pgdp, va_offset, __inittext_begin,
            __inittext_end, prot, false, root_level);

// 5. .init.data: initialization data (freed after boot)
map_segment(init_pg_dir, &pgdp, va_offset, __initdata_begin,
            __initdata_end, data_prot, false, root_level);

// 6. .data + .bss: read-write kernel data
map_segment(init_pg_dir, &pgdp, va_offset, _data, _end,
            data_prot, true, root_level);
```

### `map_segment` Helper

```c
static void map_segment(pgd_t *pg_dir, phys_addr_t *pgd, u64 va_offset,
                        void *start, void *end, pgprot_t prot,
                        bool may_use_cont, int root_level)
{
    map_range(pgd,
              ((u64)start + va_offset) & ~PAGE_OFFSET,  // virtual address
              ((u64)end + va_offset) & ~PAGE_OFFSET,     // virtual end
              (u64)start,                                 // physical address
              prot, root_level, (pte_t *)pg_dir, may_use_cont, 0);
}
```

## Two-Pass Mapping

### Why Two Passes?

When `CONFIG_RELOCATABLE` is enabled, the kernel binary contains relocations that must be patched. But the `.text` section should ultimately be read-only + executable (ROX). Solution:

```
Pass 1: Map .text as RW → write relocations → patch SCS
Pass 2: Remap .text as ROX (read-only, executable)
```

### Pass 1: Map Everything RW

```c
twopass |= enable_scs;
prot = twopass ? data_prot : text_prot;  // data_prot = RW
```

### Between Passes: Relocate and Patch

```c
if (IS_ENABLED(CONFIG_RELOCATABLE))
    relocate_kernel(kaslr_offset);  // patch instruction targets

if (enable_scs)
    scs_patch(...);  // patch PAC → SCS instructions
```

### Pass 2: Remap as ROX

```c
// Remove existing .text mappings (avoid TLB conflicts)
unmap_segment(init_pg_dir, va_offset, _stext, _etext, root_level);
dsb(ishst);
isb();
__tlbi(vmalle1);  // flush all TLBs
isb();

// Remap with final permissions
map_segment(init_pg_dir, NULL, va_offset, _stext, _etext,
            text_prot, true, root_level);    // ROX
map_segment(init_pg_dir, NULL, va_offset, __inittext_begin,
            __inittext_end, text_prot, false, root_level);  // ROX
```

Note: `pgdp = NULL` — no new page tables are allocated. The existing tables from pass 1 are reused.

## TTBR1 Switch (First)

```c
dsb(ishst);
idmap_cpu_replace_ttbr1((phys_addr_t)init_pg_dir);
```

After all segments are mapped in pass 1, TTBR1 is switched from `reserved_pg_dir` to `init_pg_dir`. Now accesses to kernel virtual addresses (0xFFFF...) will resolve.

## PGD Copy and Final TTBR1 Switch

```c
memcpy((void *)swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
dsb(ishst);
idmap_cpu_replace_ttbr1((phys_addr_t)swapper_pg_dir);
```

Only the root page (PGD, 4KB) is copied. The lower-level tables are shared:

```
swapper_pg_dir (PGD)  ──→  PUD pages (in init_pg_dir region)
                            ├── PMD pages
                            └── PTE pages
```

This means `swapper_pg_dir` is the final page table root, but it reuses all the page table pages allocated by `init_pg_dir`.

## Memory Layout After map_kernel

```
Virtual Address Space (TTBR1 / kernel):
0xFFFF_xxxx_xxxx_xxxx

   ┌────────────────────┐
   │ .head.text   (RW)  │  _text → _stext
   ├────────────────────┤
   │ .text        (ROX) │  _stext → _etext
   ├────────────────────┤
   │ .rodata      (RW)  │  __start_rodata → __inittext_begin
   ├────────────────────┤
   │ .init.text   (ROX) │  __inittext_begin → __inittext_end
   ├────────────────────┤
   │ .init.data   (RW)  │  __initdata_begin → __initdata_end
   ├────────────────────┤
   │ .data + .bss (RW)  │  _data → _end
   └────────────────────┘
```

## Protection Types

```c
PAGE_KERNEL_ROX = PTE_RDONLY | PTE_KERNEL | PTE_PXN_cleared
                  // Read-only, executable, kernel-only

PAGE_KERNEL     = PTE_WRITE | PTE_KERNEL | PTE_PXN
                  // Read-write, non-executable, kernel-only

PAGE_KERNEL_EXEC = PTE_WRITE | PTE_KERNEL
                   // Read-write + executable (rodata=off debug mode)
```

## Key Takeaway

`map_kernel()` creates the first proper kernel virtual memory layout. The two-pass approach elegantly handles the conflict between needing writable memory for relocations and wanting read-only+executable text at runtime. The bump allocator (`pgdp`) provides page table pages from the static `init_pg_dir` region, requiring no dynamic allocator.
