# `create_init_idmap()` — Build the Identity Map Page Tables

**Source:** `arch/arm64/kernel/pi/map_range.c` lines 91–112

## Function Signature

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
```

- `pg_dir` — physical address of `init_idmap_pg_dir` (the pre-allocated page table buffer)
- `clrmask` — bits to clear from page descriptors (used for LPA2 compatibility; normally 0)
- Returns: physical address of the next free byte after the page tables

## Code Walkthrough

```c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
    phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;
    pgprot_t text_prot = PAGE_KERNEL_ROX;
    pgprot_t data_prot = PAGE_KERNEL;

    pgprot_val(text_prot) &= ~clrmask;
    pgprot_val(data_prot) &= ~clrmask;

    map_range(&ptep, (u64)_stext, (u64)__initdata_begin,
              (phys_addr_t)_stext, text_prot, IDMAP_ROOT_LEVEL,
              (pte_t *)pg_dir, false, 0);

    map_range(&ptep, (u64)__initdata_begin, (u64)_end,
              (phys_addr_t)__initdata_begin, data_prot, IDMAP_ROOT_LEVEL,
              (pte_t *)pg_dir, false, 0);

    return ptep;
}
```

### Line-by-line:

**`phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;`**
- The first page of `pg_dir` is the root (PGD) table
- `ptep` starts at the second page — this is the "allocator" pointer for sub-tables
- Each time `map_range()` needs a new PUD/PMD/PTE page, it takes it from `*ptep` and advances

**`pgprot_t text_prot = PAGE_KERNEL_ROX;`**
- `PAGE_KERNEL_ROX` = Read-Only + eXecutable (for kernel text)
- Protection bits: `PTE_ATTRINDX(MT_NORMAL) | PTE_SHARED | PTE_AF | PTE_UXN | PTE_RDONLY`

**`pgprot_t data_prot = PAGE_KERNEL;`**
- `PAGE_KERNEL` = Read-Write, No-Execute (for kernel data)
- Protection bits: `PTE_ATTRINDX(MT_NORMAL) | PTE_SHARED | PTE_AF | PTE_UXN | PTE_PXN`

**`pgprot_val(text_prot) &= ~clrmask;`**
- For LPA2 (52-bit physical addresses), certain bits change meaning. `clrmask` clears those bits.
- For normal boot, `clrmask = 0`, so this is a no-op.

### First `map_range` call — Text mapping

```c
map_range(&ptep, (u64)_stext, (u64)__initdata_begin,
          (phys_addr_t)_stext, text_prot, IDMAP_ROOT_LEVEL,
          (pte_t *)pg_dir, false, 0);
```

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `pte` | `&ptep` | Allocator pointer (advances as tables are allocated) |
| `start` | `_stext` | Virtual address start (== physical, identity map) |
| `end` | `__initdata_begin` | Virtual address end |
| `pa` | `_stext` | Physical address (same as VA — identity map) |
| `prot` | `PAGE_KERNEL_ROX` | Read-only executable |
| `level` | `IDMAP_ROOT_LEVEL` | Starting page table level (depends on VA bits) |
| `tbl` | `pg_dir` | Root page table |
| `may_use_cont` | `false` | Don't use contiguous entries (simpler) |
| `va_offset` | `0` | No offset (MMU is off, no VA→PA translation needed) |

### Second `map_range` call — Data mapping

Same structure but maps `__initdata_begin` → `_end` with `PAGE_KERNEL` (read-write, no-execute).

## Why `va_offset = 0`?

When MMU is off, pointer casts to physical addresses are valid (the comment in the source says this explicitly). There's no virtual→physical offset to account for. Later, when mapping the kernel at its virtual address, `va_offset` will be non-zero.

## Why `may_use_cont = false`?

Contiguous PTE entries allow the TLB to cache a single entry covering 16 consecutive pages (64KB with 4KB pages). But contiguous entries require alignment constraints that the identity map doesn't guarantee for arbitrary kernel load addresses. Using `false` keeps it simple.

## Memory Layout After `create_init_idmap`

```
Physical Memory:
                     _stext                __initdata_begin           _end
                       │                        │                      │
                       ▼                        ▼                      ▼
  ┌────────────────────┬────────────────────────┬──────────────────────┐
  │   Kernel Text      │     Kernel Data        │                      │
  │   (code + rodata)  │  (initdata, bss, etc.) │                      │
  │   PAGE_KERNEL_ROX  │     PAGE_KERNEL        │                      │
  └────────────────────┴────────────────────────┴──────────────────────┘

Identity Map Page Tables (init_idmap_pg_dir):
  VA 0x4080_0000 ──→ PA 0x4080_0000  (text, ROX)
  VA 0x40A0_0000 ──→ PA 0x40A0_0000  (data, RW)
  ... (VA == PA for the entire kernel image)
```

## Key Takeaway

`create_init_idmap` builds the simplest possible page tables: map the kernel image to itself (VA == PA) with appropriate protections. This enables the MMU to be turned on without changing the addresses being used. The function returns a pointer past the last used byte, so the caller knows how much of the page table buffer was consumed (needed for cache maintenance).
