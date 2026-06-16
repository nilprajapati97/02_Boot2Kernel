# parse_early_param() — ARM32 vs ARM64 Design Details

## 1. Overview

`parse_early_param()` itself is **architecture-independent** — it lives in `init/main.c` and is identical on ARM32 and ARM64. The differences emerge in:
- **When** it is called relative to other setup steps
- **Which `early_param()` handlers are registered** (architecture-specific)
- **What variables those handlers set** (architecture-specific memory layout globals)

---

## 2. Call Context Comparison

| Aspect | ARM32 (`arch/arm/kernel/setup.c`) | ARM64 (`arch/arm64/kernel/setup.c`) |
|--------|----------------------------------|--------------------------------------|
| Caller | `setup_arch()` | `setup_arch()` |
| Called after | `early_ioremap_init()` | `early_ioremap_init()` |
| MMU state | ON (SCTLR.M=1, set by head.S) | ON (SCTLR_EL1.M=1, set by head.S) |
| Cache state | D-cache enabled by CP15 setup | D-cache enabled by `cpu_enable_swapper_d_cache()` |
| Called before | `early_mm_init()`, `adjust_lowmem_bounds()` | `arm64_memblock_init()` |
| `done` guard needed | Yes — arch calls it, `start_kernel` also calls it | Yes — same generic concern |

---

## 3. Architecture-Specific `early_param()` Handlers

### ARM32-Specific Handlers

| Handler | Parameter | File | Purpose |
|---------|-----------|------|---------|
| `early_vmalloc` | `vmalloc=` | `arch/arm/mm/mmu.c` | Set `vmalloc_size` — changes lowmem/highmem boundary |
| `early_mem` | `mem=` | `arch/arm/kernel/setup.c` | Call `arm_add_memory()` to trim/expand physical memory |
| `parse_tag_mem32` | — (ATAG) | `arch/arm/kernel/atags_parse.c` | ATAG-based memory specification (legacy) |

**ARM32 `early_vmalloc` detail:**
```c
static int __init early_vmalloc(char *arg)
{
    vmalloc_size = memparse(arg, NULL);

    if (vmalloc_size < SZ_16M) {
        pr_warn("vmalloc area too small, limiting to 16MiB\n");
        vmalloc_size = SZ_16M;
    }

    if (vmalloc_size > VMALLOC_END - VMALLOC_START) {
        pr_warn("vmalloc area is too big, limiting to %luMiB\n",
                (VMALLOC_END - VMALLOC_START) >> 20);
        vmalloc_size = VMALLOC_END - VMALLOC_START;
    }
    return 0;
}
early_param("vmalloc", early_vmalloc);
```

This directly affects `adjust_lowmem_bounds()` — `vmalloc_size` is used to compute `vmalloc_limit`, which sets `arm_lowmem_limit`.

### ARM64-Specific Handlers

| Handler | Parameter | File | Purpose |
|---------|-----------|------|---------|
| `early_init_dt_scan_memory` | via `mem=` | `drivers/of/fdt.c` | Parse FDT memory nodes |
| `parse_crashkernel` | `crashkernel=` | `kernel/crash_core.c` | Reserve crash kernel region |
| `kaslr_nokaslr_cmdline_parse` | `nokaslr` | `arch/arm64/kernel/kaslr.c` | Disable KASLR |
| `early_brk64` | `mem=` | `arch/arm64/mm/init.c` | Same concept, ARM64 memory add |

**ARM64 does NOT have a `vmalloc=` early param** because ARM64's virtual address space is 48-bit (or 52-bit with LPA). The vmalloc/lowmem split is not a critical 32-bit constraint — there is effectively unlimited virtual address space.

---

## 4. Virtual Memory Layout Impact of Handlers

### ARM32 Virtual Address Space (32-bit)

```
0x00000000  ┌─────────────────┐
            │  User space     │  0x00000000 – TASK_SIZE (0xBF000000)
0xBF000000  ├─────────────────┤  TASK_SIZE
            │  Modules        │  MODULES_VADDR = PAGE_OFFSET - 16MB
0xBF000000  ├─────────────────┤
            │  Lowmem         │  PAGE_OFFSET (0xC0000000) to arm_lowmem_limit
0xC0000000  ├─────────────────┤  PAGE_OFFSET
            │  Kernel code    │
            │  + data         │
            ├─────────────────┤  arm_lowmem_limit (computed by adjust_lowmem_bounds)
            │  VMALLOC area   │  vmalloc_size (default 240MB, changed by vmalloc=)
            ├─────────────────┤
            │  Highmem        │
0xFFFFFFFF  └─────────────────┘
```

`vmalloc=240M` (the default) means:
- `arm_lowmem_limit` = `VMALLOC_END - 240M - VMALLOC_OFFSET - PAGE_OFFSET + PHYS_OFFSET`

If user passes `vmalloc=128M`, lowmem grows, highmem shrinks.

### ARM64 Virtual Address Space (48-bit VA)

```
0x0000000000000000  ┌──────────────────────────┐
                    │  User space (256 TB)      │
0x0000FFFFFFFFFFFF  ├──────────────────────────┤
                    │  ... (non-canonical)      │
0xFFFF000000000000  ├──────────────────────────┤
                    │  vmalloc/ioremap (128TB)  │
                    ├──────────────────────────┤
                    │  vmemmap                  │
                    ├──────────────────────────┤
                    │  Direct linear map (1TB)  │  PAGE_OFFSET = 0xFFFF800000000000
                    ├──────────────────────────┤
                    │  Kernel image             │  KIMAGE_VADDR = MODULES_END
0xFFFFFFFFFFFFFFFF  └──────────────────────────┘
```

On ARM64, early params primarily affect **which physical memory regions are registered** (via `mem=`) rather than virtual address space layout — there is no vmalloc/lowmem 32-bit crunch.

---

## 5. `boot_command_line` Source Comparison

| | ARM32 | ARM64 |
|--|-------|-------|
| **Primary source** | FDT `/chosen/bootargs` | FDT `/chosen/bootargs` |
| **Legacy source** | ATAG_CMDLINE | None (no ATAG support) |
| **Set by** | `setup_machine_fdt()` → `early_init_dt_scan_chosen()` | `early_init_dt_scan_chosen()` |
| **Size** | `COMMAND_LINE_SIZE` = 2048 bytes | `COMMAND_LINE_SIZE` = 2048 bytes |
| **Stored in** | `boot_command_line[]` (global) | `boot_command_line[]` (global) |

ARM32 has the additional ATAG fallback path: if no FDT is present, `setup_machine_tags()` fills `boot_command_line` from ATAG structures.

---

## 6. Register-Level Differences

### ARM32 — Access to CP15 Registers

Some `early_param` handlers on ARM32 need to check CPU capabilities to decide if a parameter is valid. They use CP15:

```c
/* Example: checking LPAE support */
asm("mrc p15, 0, %0, c0, c1, 4" : "=r" (mmfr0)); /* MMFR0 */
```

### ARM64 — System Register Access

ARM64 handlers use `mrs` instruction:

```c
/* Example: checking ID_AA64MMFR0_EL1 */
u64 mmfr0 = read_cpuid(ID_AA64MMFR0_EL1);
```

ARM64 system registers are accessed at EL1 (Supervisor mode) during `setup_arch()`. No coprocessor syntax needed.

---

## 7. KASLR Interaction

### ARM32
- ARM32 does **not** have KASLR in the mainline kernel.
- No `nokaslr` early param exists for ARM32.
- The `parse_early_param()` call has no KASLR-related side effects.

### ARM64
- ARM64 has KASLR since kernel 4.6.
- `kaslr_nokaslr_cmdline_parse` is registered via `early_param("nokaslr", ...)`.
- If `nokaslr` is present on the command line, `parse_early_param()` sets `__nokaslr_early_buf` which is checked during `kaslr_init()`.
- The KASLR randomization has already **happened before** `parse_early_param()` is called (it runs in `init/main.c:start_kernel()` → KASLR is applied in `head.S`). The `nokaslr` param prevents the **next** boot from applying it, or can disable relocations.

---

## 8. earlycon / earlyprintk Handling

| | ARM32 | ARM64 |
|--|-------|-------|
| **earlycon param** | `earlycon=uart8250,mmio,0x1C090000` | `earlycon=pl011,0x09000000` |
| **Handler** | `drivers/tty/serial/earlycon.c: param_setup_earlycon` | Same (generic) |
| **Hardware** | UART mapped at physical address before MMU | Same — but no-MMU phase shorter on ARM64 |
| **earlyprintk** | `arch/arm/kernel/early_printk.c` | Not supported; use earlycon |
| **Output method** | Direct UART MMIO write via fixmap | `earlycon` through UART mapped in fixmap |

**Key difference:** ARM32 supports the legacy `earlyprintk=` which writes directly without the earlycon framework. ARM64 uses only the structured `earlycon=` interface.

---

## 9. Comparison Table — parse_early_param() Context

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Function location | `init/main.c` (generic) | `init/main.c` (generic) |
| `done` guard | Yes | Yes |
| `tmp_cmdline` copy | Yes | Yes |
| Memory constraint of handlers | Must not allocate (pre-memblock) | Must not allocate (pre-memblock) |
| vmalloc= handler | Yes — critical for 32-bit VA layout | No (not needed) |
| mem= handler | `early_mem()` → `arm_add_memory()` | Different path via `arm64_memblock_init()` override |
| nokaslr support | No | Yes |
| earlyprintk | Yes (legacy) | No (earlycon only) |
| ATAG fallback cmdline | Yes | No |
| console= → earlycon alias | Yes (generic) | Yes (generic) |

---

## 10. ARM64 `early_param` Flow Specific Nuance

On ARM64, `parse_early_param()` is called from `setup_arch()` in `arch/arm64/kernel/setup.c`:

```c
void __init setup_arch(char **cmdline_p)
{
    ...
    *cmdline_p = boot_command_line;

    kaslr_init();                    /* KASLR setup (reads early vars) */
    ...
    parse_early_param();             /* ← HERE, same generic function */
    ...
    arm64_memblock_init();           /* memblock fully configured */
    paging_init();                   /* page tables */
    ...
}
```

ARM64's `parse_early_param()` feeds into `arm64_memblock_init()` (equivalent of ARM32's `arm_memblock_init()`). The exact set of effective early parameters differs due to the 64-bit address space, but the framework is identical.
