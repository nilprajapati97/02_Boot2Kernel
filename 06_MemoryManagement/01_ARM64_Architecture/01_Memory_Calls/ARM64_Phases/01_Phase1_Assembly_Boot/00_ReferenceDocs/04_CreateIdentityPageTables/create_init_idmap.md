# `create_init_idmap` — Identity-Mapped Page Table Creation

## 1. Call Chain: From Assembly to C

```
Boot entry (head.S)
  │
  ├── primary_entry()                        [head.S:85]
  │     │
  │     ├── bl record_mmu_state              saves MMU state in x19
  │     ├── bl preserve_boot_args            saves FDT pointer (x0→x21) and boot args
  │     │
  │     ├── adrp x1, early_init_stack        set up temporary stack
  │     ├── mov  sp, x1
  │     ├── mov  x29, xzr                    null frame pointer
  │     │
  │     ├── adrp x0, __pi_init_idmap_pg_dir  ← x0 = physical address of page table storage
  │     ├── mov  x1, xzr                     ← x1 = 0 (clrmask = no bits to clear)
  │     ├── bl   __pi_create_init_idmap      ← CALLS the C function
  │     │
  │     ├── [cache invalidation/cleaning]
  │     ├── bl init_kernel_el
  │     ├── bl __cpu_setup
  │     └── b  __primary_switch
  │              │
  │              ├── adrp x2, __pi_init_idmap_pg_dir  ← loads idmap into TTBR0
  │              ├── bl __enable_mmu                  ← TURNS ON MMU
  │              └── bl __pi_early_map_kernel         ← creates full kernel mapping
```

### Files involved:
| File | Role |
|------|------|
| `arch/arm64/kernel/head.S` | Assembly boot entry; calls `__pi_create_init_idmap` |
| `arch/arm64/kernel/pi/map_range.c` | C implementation of `create_init_idmap()` and `map_range()` |
| `arch/arm64/kernel/pi/pi.h` | Header declaring `create_init_idmap()` and `map_range()` |
| `arch/arm64/kernel/pi/Makefile` | Build rules; adds `__pi_` prefix to all symbols via `--prefix-symbols=__pi_` |
| `arch/arm64/kernel/vmlinux.lds.S` | Linker script; allocates `init_idmap_pg_dir` memory |
| `arch/arm64/include/asm/kernel-pgtable.h` | Macros for page table sizing (`INIT_IDMAP_DIR_SIZE`, `IDMAP_ROOT_LEVEL`, etc.) |

---

## 2. The `__pi_` Prefix Mechanism

The code in `arch/arm64/kernel/pi/` is **position-independent (PI)**. The build system (`pi/Makefile` line 27) uses:

```makefile
OBJCOPYFLAGS := --prefix-symbols=__pi_
```

This renames every symbol in the object file with a `__pi_` prefix. So:
- C function `create_init_idmap()` → linker symbol `__pi_create_init_idmap`
- C function `early_map_kernel()` → linker symbol `__pi_early_map_kernel`
- External symbol `init_idmap_pg_dir` → referenced as `__pi_init_idmap_pg_dir`

This ensures the PI code uses **PC-relative addressing only** (no absolute virtual addresses), which is essential because the MMU is off at this point.

---

## 3. Memory Allocation for the Page Tables

In the linker script (`vmlinux.lds.S`, line 263):

```ld
__pi_init_idmap_pg_dir = .;
. += INIT_IDMAP_DIR_SIZE;
__pi_init_idmap_pg_end = .;
```

This reserves a contiguous block of memory inside the kernel image's `.initdata` segment.

### How `INIT_IDMAP_DIR_SIZE` is calculated (`kernel-pgtable.h`):

```c
#define IDMAP_VA_BITS       48
#define IDMAP_LEVELS        ARM64_HW_PGTABLE_LEVELS(IDMAP_VA_BITS)
#define IDMAP_ROOT_LEVEL    (4 - IDMAP_LEVELS)

#define INIT_IDMAP_DIR_PAGES    (EARLY_PAGES(INIT_IDMAP_PGTABLE_LEVELS, KIMAGE_VADDR, kimage_limit, 1))
#define INIT_IDMAP_DIR_SIZE     ((INIT_IDMAP_DIR_PAGES + EARLY_IDMAP_EXTRA_PAGES) * PAGE_SIZE)
```

For a typical 4K page, 4-level (48-bit VA) configuration:
- `IDMAP_LEVELS` = 4
- `IDMAP_ROOT_LEVEL` = 0 (PGD)
- The size accommodates: **1 PGD page + enough pages for PUD, PMD, and PTE levels** to cover the kernel image from `KIMAGE_VADDR` to `kimage_limit`.

---

## 4. `create_init_idmap()` — Detailed Walkthrough

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

### Step-by-step:

**a) Arguments:**
- `pg_dir`: Physical address of `init_idmap_pg_dir` — the pre-allocated page table memory
- `clrmask`: Bits to clear from protection values. Called with `0` from `primary_entry` (no bits cleared). Called with `PTE_SHARED` during LPA2 remapping.

**b) Page table allocator setup:**
```c
phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;
```
The first `PAGE_SIZE` (4KB) of `pg_dir` is the **root PGD table**. `ptep` is a bump allocator pointer — it starts right after the PGD and advances every time a new subordinate page table page is needed.

**c) Protection attributes:**
```
text_prot = PAGE_KERNEL_ROX  → Read-Only + Executable (for kernel code)
data_prot = PAGE_KERNEL      → Read-Write (for kernel data)
```
If `clrmask` is non-zero (LPA2 case), specific attribute bits are cleared.

**d) Two `map_range()` calls create the identity map:**

| Call | Virtual Range | Physical Range | Protection | What it maps |
|------|--------------|----------------|------------|--------------|
| 1st | `_stext` → `__initdata_begin` | Same as virtual | `PAGE_KERNEL_ROX` | Kernel text, rodata, init text |
| 2nd | `__initdata_begin` → `_end` | Same as virtual | `PAGE_KERNEL` | Init data, data, BSS |

**KEY: Virtual address == Physical address** — this is the identity mapping. Since the MMU is off, symbol addresses resolved by the linker are physical addresses. Both the VA and PA arguments to `map_range()` are the same value.

**e) Return value:**
Returns `ptep` — the physical address just past the last allocated page table page. This is used by the caller to know how much memory to cache-invalidate.

---

## 5. `map_range()` — The Recursive Page Table Builder

```c
void map_range(phys_addr_t *pte, u64 start, u64 end, phys_addr_t pa,
               pgprot_t prot, int level, pte_t *tbl, bool may_use_cont,
               u64 va_offset)
```

### Memory layout it creates:

```
pg_dir (root)          Level 0 (PGD)
  ┌─────────────────┐
  │ Entry → PUD pg  │──┐
  │ ...              │  │
  └─────────────────┘  │
                        ▼
  ptep[0]              Level 1 (PUD)
  ┌─────────────────┐
  │ Entry → PMD pg  │──┐
  │ ...              │  │
  └─────────────────┘  │
                        ▼
  ptep[1]              Level 2 (PMD)
  ┌─────────────────┐
  │ Block desc      │ → 2MB block mapping (PA | protval | PMD_TYPE_SECT)
  │ Block desc      │ → 2MB block mapping
  │ ...              │
  └─────────────────┘
```

### Algorithm:

1. **Index into table**: Compute which entry in `tbl` covers `start`:
   ```c
   tbl += (start >> (lshift + PAGE_SHIFT)) % PTRS_PER_PTE;
   ```

2. **For each chunk** from `start` to `end`:
   - If the range is **not aligned** to this level's block size, or we're at level 0/1:
     - **Allocate a next-level table** from `ptep` (bump allocator):
       ```c
       *tbl = __pte(__phys_to_pte_val(*pte) | PMD_TYPE_TABLE | PMD_TABLE_UXN);
       *pte += PTRS_PER_PTE * sizeof(pte_t);
       ```
     - **Recurse** into `map_range()` at `level + 1`
   - If aligned at level 2 (PMD):
     - Install a **2MB block descriptor** directly:
       ```c
       *tbl = __pte(__phys_to_pte_val(pa) | protval);
       ```
       where `protval` includes `PMD_TYPE_SECT` (block descriptor bit)

3. **Advance** `pa`, `start`, and `tbl` to the next entry.

### Concrete example (4KB pages, 4-level tables):

For a kernel loaded at physical address `0x4000_0000`:
```
map_range(ptep, 0x4000_0000, 0x4200_0000, 0x4000_0000, ROX, level=0, pg_dir)
  │
  ├─ Level 0 (PGD): entry[0] → allocate PUD page from ptep
  │    │
  │    └─ Level 1 (PUD): entry[1] → allocate PMD page from ptep
  │         │
  │         └─ Level 2 (PMD):
  │              entry[512] = block(0x4000_0000 | SECT | ROX)  ← 2MB block
  │              entry[513] = block(0x4020_0000 | SECT | ROX)  ← 2MB block
  │              ...
```

The result: VA `0x4000_0000` → PA `0x4000_0000` (identity mapped).

---

## 6. How the Identity Map Gets Used

After `create_init_idmap()` returns to `primary_entry`:

1. **Cache maintenance** (lines 99-112): If MMU was off, invalidate dcache for the page table region. If MMU was on, clean the idmap text to PoC.

2. **`__primary_switch`** (line 126): Loads the idmap into `TTBR0_EL1`:
   ```asm
   adrp    x2, __pi_init_idmap_pg_dir
   bl      __enable_mmu
   ```
   `__enable_mmu` writes `x2` to `TTBR0_EL1` and sets `SCTLR_EL1` to enable the MMU.

3. **After MMU is on**: The CPU's PC still holds the same physical address, but now the MMU translates it. Because VA == PA in the identity map, execution continues seamlessly.

4. **`__pi_early_map_kernel`** then creates the **real kernel mapping** (VA at `KIMAGE_VADDR` → PA) in `init_pg_dir`/`swapper_pg_dir`, and the identity map is eventually discarded.

---

## 7. Memory Map Summary

```
init_idmap_pg_dir (INIT_IDMAP_DIR_SIZE bytes)
┌────────────────────────────────┐  ← __pi_init_idmap_pg_dir
│  Page 0: PGD (root table)      │
├────────────────────────────────┤  ← ptep starts here
│  Page 1: PUD (level 1)         │  allocated by map_range()
├────────────────────────────────┤
│  Page 2: PMD (level 2)         │  allocated by map_range()
├────────────────────────────────┤
│  ... more pages if needed ...  │
├────────────────────────────────┤  ← ptep ends here (return value)
│  Unused reserved space         │
└────────────────────────────────┘  ← __pi_init_idmap_pg_end
```

### Key identity mappings created:

| Virtual Range | Physical Range | Protection | Content |
|--------------|----------------|------------|---------|
| `_stext` → `__initdata_begin` | Same | `PAGE_KERNEL_ROX` (RO+Exec) | Kernel text + rodata + init text |
| `__initdata_begin` → `_end` | Same | `PAGE_KERNEL` (RW) | Init data + data + BSS |
