# `__pi_early_map_kernel` — Complete Walkthrough

**Source:** `arch/arm64/mm/pi/map_kernel.c`  
**Called from:** `__primary_switch` as a C function  
**Purpose:** Build the page tables that the MMU will use immediately after enable

---

## 0. What This Function Must Accomplish

Before `__enable_mmu` can run, `swapper_pg_dir` and `__pi_init_idmap_pg_dir`
must contain valid page table entries. This function builds both:

1. **`swapper_pg_dir`**: The kernel's permanent virtual address mapping (TTBR1)
   - Maps the entire kernel image at `0xFFFF_8000_0000_0000 + offset`
   - Maps the FDT blob
   - Respects section permissions (RX for text, RO for rodata, RW for data)

2. **`__pi_init_idmap_pg_dir`**: The identity map (TTBR0)
   - Maps kernel image at PA = VA
   - 2MB block entries (3-level walk) for efficiency
   - Used only during the PA=VA window after MMU enable

---

## 1. Function Signature and Calling Convention

```c
// arch/arm64/mm/pi/map_kernel.c
asmlinkage void __init __pi_early_map_kernel(
    unsigned long fdt_pa,      // x0: physical address of FDT blob
    unsigned long va_base,     // (may be embedded or computed)
    ...
)
```

`asmlinkage` means the function does NOT use the register argument passing
of the calling convention — arguments are passed in registers x0, x1, x2...
(AAPCS64). For kernel functions, `asmlinkage` typically means "don't pass
args in registers but use the stack" (x86 convention), but on ARM64 it's
a no-op — AAPCS64 always uses registers for the first 8 arguments.

---

## 2. Early Allocator — How Page Table Memory Is Obtained

Building a 4-level page table requires memory for intermediate tables
(PGD, PUD, PMD pages). This memory cannot come from the kernel's normal
allocator (not initialized yet). Instead, `__pi_early_map_kernel` uses a
simple linear allocator from a pre-reserved region:

```c
// Static early page table storage:
static struct {
    pgd_t pgd[PTRS_PER_PGD];           // 512 entries × 8B = 4096B = 1 page
    pud_t pud[N][PTRS_PER_PUD];        // Multiple PUD pages as needed
    pmd_t pmd[N][PTRS_PER_PMD];        // Multiple PMD pages as needed
} __pi_swapper_tables;
// (exact layout differs; this is conceptual)
```

OR using `init_pg_dir` / `init_pg_end` reserved in the linker script:

```ld
// vmlinux.lds.S:
    INIT_PG_DIR
    // expands to:
    . = ALIGN(PAGE_SIZE);
    init_pg_dir = .;
    . += INIT_DIR_SIZE;
    init_pg_end = .;
```

`INIT_DIR_SIZE` is computed at link time to be large enough for all needed
page table pages (PGD + all PUDs + all PMDs for the kernel image size).

The allocator is a simple bump pointer:
```c
static void *early_pgtable_alloc(int shift)
{
    static phys_addr_t next_pa = __pa_symbol(init_pg_dir);
    phys_addr_t result = next_pa;
    next_pa += PAGE_SIZE;
    assert(next_pa <= __pa_symbol(init_pg_end));
    return (void *)result;
}
```

---

## 3. Mapping the Kernel Image

The core mapping logic creates PMD-level (2MB) block entries for the kernel:

```c
// Simplified mapping call:
map_kernel_range(
    __pa_symbol(_stext),         // PA start of kernel text
    __pa_symbol(_etext) - __pa_symbol(_stext),  // size
    PAGE_KERNEL_ROX,             // RO + X permissions
    &swapper_pg_dir
);

map_kernel_range(
    __pa_symbol(__start_rodata),
    rodata_size,
    PAGE_KERNEL_RO,              // RO, no execute
    &swapper_pg_dir
);

map_kernel_range(
    __pa_symbol(_data),
    data_size,
    PAGE_KERNEL,                 // RW, no execute
    &swapper_pg_dir
);
```

### `map_kernel_range` Internal Logic

```c
static void map_range(
    phys_addr_t pa_start, phys_addr_t pa_end,
    unsigned long va_start,
    pgprot_t prot,
    pgd_t *pgdir)
{
    // For each 2MB-aligned chunk:
    pmd_t *pmd = alloc_pmd_entry(pgdir, va_start);
    set_pmd(pmd, pfn_pmd(pa_start >> PAGE_SHIFT, prot));
    // set_pmd sets bits[1:0] = 0b01 (BLOCK) for PMD-level entry
    va_start += PMD_SIZE;  // advance by 2MB
    pa_start += PMD_SIZE;
}
```

**2MB alignment requirement:** The kernel image is loaded at a 2MB-aligned
physical address by the bootloader. If it were not, some regions would straddle
2MB boundaries and require 4KB page mappings (one more level of tables).

---

## 4. Mapping the FDT

```c
// Map the FDT blob into the kernel VA space
// FDT is at fdt_pa, size is fdt_totalsize(fdt_va)
map_fdt(fdt_pa, fdt_size);
// Creates RO mapping at VA = phys_to_virt(fdt_pa)
```

The FDT mapping uses 4KB pages (not 2MB blocks) because the FDT size is
typically 32KB–128KB and may not be 2MB-aligned.

---

## 5. KASLR Integration

With KASLR enabled, the kernel computes the virtual-to-physical offset:

```c
// The kernel is loaded at a randomized PA:
// kimage_pa = randomized load address (e.g., 0x5200_0000)
// kimage_va = _text (compile-time VA, e.g., 0xFFFF_8000_8000_0000)

// The offset that converts VA → PA:
kimage_voffset = (unsigned long)_text - kimage_pa;
// kimage_voffset = 0xFFFF_8000_8000_0000 - 0x5200_0000
//               = 0xFFFF_7FFF_2E00_0000 (some large negative-ish value)

// Store for later use by __pa()/__va() macros:
*(unsigned long *)__pi_kimage_voffset = kimage_voffset;
```

The `__pi_` prefix on `__pi_kimage_voffset` means this is a PI-accessible
version of the `kimage_voffset` global. After the MMU is on, `kimage_voffset`
(the non-PI version at a kernel VA) holds the same value.

---

## 6. The Identity Map (`__pi_init_idmap_pg_dir`)

```c
// Build the identity map (PA = VA mapping) for the PA=VA window
__pi_create_init_idmap(
    kimage_pa,           // physical start of kernel
    kimage_pa + kimage_size,  // physical end
    &__pi_init_idmap_pg_dir
);
```

This creates 2MB block entries mapping:
```
VA range [kimage_pa, kimage_pa + size) → PA range [kimage_pa, kimage_pa + size)
```

The identity map uses only 3 levels (PGD→PUD→PMD with 2MB blocks), not 4,
because 2MB blocks terminate at the PMD level. This reduces the table size and
PTW latency.

---

## 7. Memory Layout After `__pi_early_map_kernel`

```
Physical Memory Map:

┌──────────────────────────────────────────────────────────────────────┐
│ init_pg_dir region (from vmlinux.lds.S):                            │
│   PGD for swapper_pg_dir       (4 KB)                               │
│   PUD entries for kernel VA    (4 KB each as needed)               │
│   PMD entries for kernel VA    (4 KB each as needed)               │
│   Total: ~10-50 pages depending on kernel size                      │
├──────────────────────────────────────────────────────────────────────┤
│ __pi_init_idmap_pg_dir (BSS or separate):                           │
│   PGD for identity map         (4 KB)                               │
│   PUD entries                  (4 KB each)                          │
│   PMD entries (2MB blocks)     (4 KB each)                          │
│   Total: ~3-6 pages                                                  │
├──────────────────────────────────────────────────────────────────────┤
│ Kernel image (.text, .rodata, .data, .bss)                          │
├──────────────────────────────────────────────────────────────────────┤
│ FDT blob (placed by bootloader)                                      │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 8. The Position-Independent Execution Context

The most subtle constraint: this function runs **before the MMU is on**, so
all code must work with physical addresses.

Position-independent means:
- `&swapper_pg_dir` is computed using `adrp`/`add` (PC-relative, gives PA)
- Global variable reads/writes use GOT with PC-relative GOT access
- The return value (if any) is a physical address or register value

After this function returns, `swapper_pg_dir` and `__pi_init_idmap_pg_dir` are
fully populated page tables in physical memory, ready for the TTBR registers.

---

## 9. Why This Is a C Function, Not Assembly

Building 4-level page tables correctly requires:
- Nested loops over VA ranges
- Arithmetic on 64-bit addresses with masking
- Memory allocation (bump pointer)
- Conditional logic (KASLR on/off, block vs. page mapping)

Implementing this in assembly would be error-prone and hard to maintain. The
`__pi_` prefix and position-independent compilation allow C code to run safely
before the MMU is enabled.

All ARM64-specific details (page table format, MAIR attribute indices, block
entry bit patterns) are encoded in kernel headers (`pgtable.h`, `pgtable-hwdef.h`)
and used normally by the C code.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses 64-bit (Aarch64) ELF binaries with RELA relocations. A RELA entry encodes (offset, symbol, addend): the linker writes the relocation record; at load time, the loader computes the final address as symbol_value + addend and writes it to the offset location. For position-independent code (PIC/PIE), all absolute address references are expressed as relocations. The CPU itself is not involved in relocation; it simply executes whatever instructions and data are at the final addresses after the loader has applied all relocations.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 kernel is linked as a PIE (Position Independent Executable) when KASLR is enabled. All absolute symbol references in the kernel become RELA relocation entries in the .rela.dyn ELF section. At boot, before the MMU is enabled, __pi_relocate_kernel (arch/arm64/kernel/pi/relocate.c) iterates over every RELA entry and applies the delta (actual_load_PA - link_PA) to each relocation site. This is why all boot-path code uses the __pi_ prefix: it is position-independent and can run before relocations are applied.

### Memory Perspective (ARMv8 Memory Model)
Applying ELF relocations means modifying kernel data at physical addresses. These writes go through the D-cache (if enabled) or directly to memory (if not). After applying all relocations, the kernel performs a D-cache flush (DC CIVAC range) and an I-cache invalidation (IC IVAU range) for any text sections that were modified. This ensures the I-cache does not serve stale pre-relocation instructions. The ARMv8 memory model requires the dsb + isb sequence after I-cache maintenance to guarantee that the pipeline fetches the updated instructions.