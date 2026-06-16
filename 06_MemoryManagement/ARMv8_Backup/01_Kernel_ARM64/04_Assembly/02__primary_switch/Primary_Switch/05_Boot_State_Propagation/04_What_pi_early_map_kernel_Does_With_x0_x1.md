# What `__pi_early_map_kernel` Does With x0 and x1

**Function signature:** `void __pi_early_map_kernel(unsigned long fdt_pa, unsigned long va_base, ...)`  
**Called from:** `__primary_switch` via `bl __pi_early_map_kernel`  
**Source:** `arch/arm64/mm/pi/map_kernel.c`

---

## 0. The Critical Handoff

In `__primary_switch`:

```asm
mov     x0, x21         // x0 = FDT physical address
// (x1 may be another argument or not used, depending on kernel version)
bl      __pi_early_map_kernel
```

AAPCS64 argument passing:
- `x0` = first argument
- `x1` = second argument
- `x2`–`x7` = further arguments

The return value is in `x0` (if it returns a value — `void` function returns nothing).

---

## 1. What `__pi_early_map_kernel` Returns via x0 and x1

According to the AAPCS64:
- A `void` function does not write to `x0` on return (the value is undefined)
- The CALLING function cannot rely on `x0` after calling a `void` function

In `__primary_switch`, after `bl __pi_early_map_kernel` returns, the code
immediately sets up arguments for `__enable_mmu`:

```asm
// After bl __pi_early_map_kernel:
adrp    x1, swapper_pg_dir      // x1 = new value (overwrites any return)
adrp    x2, __pi_init_idmap_pg_dir  // x2 = new value
// x0 is loaded from SCTLR value or recomputed
```

So `x0` after `__pi_early_map_kernel` is not used — it is immediately overwritten.

---

## 2. What `__pi_early_map_kernel` Actually Does

The `__pi_` prefix means **position-independent** — this function can run at
any physical address without knowing the kernel's virtual address (see the
`PI_Prefix_Explained` document for full details).

### Step 1: Parse FDT for Memory Reservations

```c
// arch/arm64/mm/pi/map_kernel.c
void __init __pi_early_map_kernel(unsigned long fdt_pa, ...)
{
    struct fdt_header *fdt = (struct fdt_header *)fdt_pa;
    
    // Scan FDT /memreserve/ entries
    // These are PA ranges the bootloader has reserved (e.g., PSCI areas, EFI runtime)
    // The kernel must not map over these without proper handling
    early_fdt_map(fdt_pa, ...);
```

### Step 2: Map the Kernel Image

```c
    // Determine where the kernel is loaded in physical memory
    unsigned long kernel_pa = __pa_symbol(_text);  // _text physical address
    unsigned long kernel_size = _end - _text;       // total kernel size

    // Map the kernel in swapper_pg_dir (TTBR1's target)
    __pi_map_range(..., kernel_pa, kernel_size, ...);
    // Creates VA→PA mappings in swapper_pg_dir for all kernel sections
```

The mapping respects section permissions:
- `.text`: R-X (read + execute, no write)
- `.rodata`: R-- (read only)
- `.data`: RW- (read + write, no execute)
- `.bss`: RW-

### Step 3: Handle KASLR

```c
    // If KASLR is active, the kernel was loaded at a randomized PA
    // The VA→PA offset (kimage_voffset) must be computed and stored
    
    kimage_voffset = (unsigned long)_text - kernel_pa;
    // kimage_voffset = VA of _text - PA of _text
    // Used by __pa()/__va() macros for all subsequent address translations
```

### Step 4: Map the FDT

```c
    // The FDT is at fdt_pa, but the kernel will access it via a VA
    // Map the FDT into the kernel VA space
    __pi_map_range(..., fdt_pa, fdt_size, PAGE_READONLY, ...);
```

### Step 5: Map Device Trees for EFI

```c
    // On EFI systems, additional firmware tables need mapping
    // (EFI system table, UEFI memory map)
    // This happens via efi_early_map() if CONFIG_EFI is enabled
```

---

## 3. The `__pi_` Prefix — Position Independence

`__pi_early_map_kernel` calls `__pi_create_pgd_next_table` and other `__pi_`
prefixed functions. They are all compiled with:

```
-fpic / -fPIC   (Position Independent Code)
-fpie / -fPIE   (Position Independent Executable)
```

This ensures:
- No hardcoded absolute addresses in the code
- All global variable accesses use GOT (Global Offset Table) or PC-relative addressing
- The code runs correctly at any physical load address

Without position independence, the page table building code would try to access
global variables using compiled-in VAs (which aren't mapped yet before the MMU
is on) → crash.

---

## 4. The `swapper_pg_dir` Output

After `__pi_early_map_kernel` returns, `swapper_pg_dir` (in physical memory)
contains a complete 4-level page table tree that maps the kernel:

```
swapper_pg_dir structure (for 48-bit VA, 4KB granule):

PGD (1 page = 4KB = 512 entries):
    Entry[256] → PUD table PA    (index 256 = VA[47:39] for 0xFFFF_8000_...)
    All others = INVALID

PUD (1 page):
    Entry[0] → PMD table PA
    
PMD (1 or more pages):
    Entry[N]   → 2MB block PA (for .text section at 2MB-aligned PA)
    Entry[N+1] → 2MB block PA (for .rodata)
    Entry[N+2] → 2MB block PA (for .data)
    ...
```

**TTBR1 → swapper_pg_dir:**

After `load_ttbr1 x1, x1, x3` in `__enable_mmu`, `TTBR1_EL1` points to
`swapper_pg_dir`. Any TTBR1-range VA translation walks this table.

---

## 5. `__pi_init_idmap_pg_dir` — Also Built Here

Within `__pi_early_map_kernel`, the identity map is also (re)built:

```c
// Build TTBR0 identity map (PA=VA for kernel image)
__pi_create_init_idmap(kernel_pa, kernel_size, ...);
// Writes to __pi_init_idmap_pg_dir
```

This is the table used for TTBR0 during the PA=VA window after MMU enable.
See document `02___pi_init_idmap_pg_dir_Deep_Dive.md` for details.

---

## 6. Return Path and Post-Call State

```asm
// Before bl __pi_early_map_kernel:
//   swapper_pg_dir = EMPTY (all zeros)
//   __pi_init_idmap_pg_dir = EMPTY
//   x20 = boot mode
//   x21 = FDT PA

bl      __pi_early_map_kernel   // C function call

// After bl __pi_early_map_kernel returns:
//   swapper_pg_dir = FULL (kernel mapped at kernel VAs)
//   __pi_init_idmap_pg_dir = FULL (identity map built)
//   x20 = boot mode (preserved by AAPCS64) ✓
//   x21 = FDT PA (preserved by AAPCS64) ✓
//   x0 = undefined (void function, don't use)
//   x1-x18 = potentially clobbered (caller-saved)
```

The function effectively "fills in" the page tables that the later
`__enable_mmu` will point the TTBR registers at.

---

## 7. What Happens If `__pi_early_map_kernel` Fails

The function does not return an error code. If it encounters a problem (e.g.,
cannot allocate enough memory for page table pages from the early allocator),
it either:
1. Silently produces incomplete page tables → MMU enable faults when PTW reads
   an INVALID entry
2. Calls `early_fixmap_init` or similar that might panic

In practice, failure here means the machine is not supported or has a
misconfigured memory map. The kernel will crash immediately after MMU enable
with a translation fault. The crash address will be in the kernel `.text`
section VA range → "Unable to handle kernel NULL pointer dereference at
virtual address 0xFFFF...".

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
__pi_early_map_kernel is the boot-time function that populates the initial kernel page tables (init_pg_dir, __idmap_pg_dir). It runs before the MMU is enabled, using physical addresses. The CPU executes it in the flat PA space: all pointer dereferences are direct PA accesses. The function must use the position-independent __pi_ variant because the kernel image has not been relocated yet -- absolute symbol references would be wrong if KASLR placed the image at a different PA than the link address.

### Kernel Perspective (Linux ARM64)
__pi_early_map_kernel (arch/arm64/mm/mmu.c, with __pi_ prefix for PI access) does:
1. Creates the identity map (VA==PA) for the .idmap.text section in __idmap_pg_dir.
2. Creates the kernel text/data/bss mappings at the randomized VA in init_pg_dir.
3. Maps the FDT at a fixmap address.
4. Sets up the fixmap page tables.
On return, TTBR0_EL1 = __idmap_pg_dir (PA) and TTBR1_EL1 = init_pg_dir (PA) are written in __primary_switch before calling __enable_mmu.

### Memory Perspective (ARMv8 Memory Model)
The page tables created by __pi_early_map_kernel reside in Normal Non-Cacheable (or write-through, implementation-defined) memory during their construction -- because the D-cache is not yet enabled (SCTLR_EL1.C=0). All writes to page-table memory go directly to DRAM without caching. After the page tables are complete, a dsb ish ensures all stores reach the memory subsystem before the page-table walker is allowed to read them (when the MMU is turned on). This is critical: without the dsb, the walker might see stale DRAM state and produce wrong translations.