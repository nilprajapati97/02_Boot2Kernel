# KASLR Impact on Module Loading

## The Module Loading Challenge

Kernel modules (`.ko` files) are loaded AFTER boot, into the vmalloc area. They
use PC-relative addressing and branch instructions that require being within a
certain range of the kernel text.

ARM64 branch instruction range: `bl`, `b` have ±128 MB range.

If the module is loaded more than 128 MB from the kernel text, direct branches
won't reach — requiring trampolines (PLT entries):

```c
// arch/arm64/kernel/module.c
static bool in_init(const struct module *mod, void *loc)
{
    return (unsigned long)loc - (unsigned long)mod->init_layout.base <
           mod->init_layout.size;
}

// Modules are allocated in the module region (within ±128 MB of _etext):
#define MODULES_VADDR   _etext - MODULES_VSIZE
#define MODULES_VEND    _etext + MODULES_VSIZE
#define MODULES_VSIZE   SZ_128M  // 128 MB
```

KASLR randomizes `_etext`, which randomizes the module area, which means modules
land at different addresses every boot. This is intentional — module addresses
are also randomized.

---

## How Modules Handle KASLR

1. **Module PLT (Procedure Linkage Table)**: For calls from module to kernel
   that are out of branch range, a PLT trampoline is used:
   ```asm
   // module_code.o calling kernel_function:
   bl      plt_entry          // +128 MB max
   ...
   plt_entry:
       ldr  x16, =kernel_function_addr   // load absolute VA
       br   x16                          // indirect branch
   ```
   The absolute VA in the PLT is patched at module load time with the correct
   (KASLR-adjusted) kernel function address.

2. **Relocation types used**: `R_AARCH64_CALL26` for short-range, `R_AARCH64_PLT32`
   for cross-module long-range calls.

3. **Module GOT (Global Offset Table)**: Module global variable accesses use GOT
   entries patched at load time.

---

## KASLR Module Address Randomization

Modules have their OWN randomization separate from the kernel image:

```c
// arch/arm64/kernel/module.c
void *module_alloc(unsigned long size)
{
    u64 module_alloc_end = module_alloc_base + MODULES_VSIZE;
    gfp_t gfp_mask = GFP_KERNEL;
    void *p;

    // module_alloc_base itself is randomized via KASLR:
    // module_alloc_base = _etext - MODULES_VSIZE + kaslr_module_offset
    
    p = __vmalloc_node_range(size, MODULE_ALIGN,
                             module_alloc_base, module_alloc_end,
                             gfp_mask, PAGE_KERNEL, 0,
                             NUMA_NO_NODE, __builtin_return_address(0));
    ...
}
```

So both the kernel image AND each module have randomized load addresses.
`kimage_voffset` is for the kernel image only; modules use page-table-based PA conversion.

---

## Impact on Debugging Modules with KASLR

```bash
# After loading a module, find its load address:
$ cat /proc/modules | grep my_module
my_module 12345 0 - Live 0xffff000001234000 (O)

# The 0xffff000001234000 is the module text VA
# This is NOT kimage_voffset territory (it's in vmalloc at 0xffff000000000000)

# To get module PA (for DMA debugging):
crash> mod my_module
NAME    TEXT_SIZE   TEXT_VADDR          DATA_SIZE   DATA_VADDR
my_module ...       0xffff000001234000  ...         ...

# PA via page table:
crash> vtop 0xffff000001234000
VIRTUAL           PHYSICAL
ffff000001234000  4a567000    # vmalloc PA lookup, NOT kimage_voffset
```

For modules, `vmalloc_to_page(va) → page_to_phys(page)` must be used, not
`kimage_voffset`-based `__pa()`.

---

## KASLR and ftrace/kprobes

Dynamic tracing (ftrace, kprobes) modifies kernel code at runtime by inserting
`bl` instructions into functions. With KASLR:

1. ftrace stores the addresses of functions to patch at boot
2. These addresses are VA addresses (post-KASLR)
3. Patching uses the runtime VA (not a compile-time address)
4. The `kimage_voffset` is needed to convert `kallsyms` symbol addresses to
   runtime VAs: `runtime_va = compile_time_va + kaslr_va_offset`
5. `kaslr_va_offset = kimage_voffset - PAGE_OFFSET`

Without correct `kimage_voffset`, ftrace symbol resolution would fail after boot.

---

## `CONFIG_RANDOMIZE_BASE=n` Performance Impact

When KASLR is disabled (e.g., for production systems where boot time matters):
- No entropy computation at boot
- No relocation of the kernel image
- `kimage_voffset = PAGE_OFFSET` exactly
- Faster boot (~5–10 ms less for large kernels on slow devices)
- `__pa()` could theoretically be optimized to use `PAGE_OFFSET` constant

Linux still uses `kimage_voffset` (runtime variable) even with KASLR disabled,
for code simplicity — the `sub x4, x4, x0` in `__primary_switched` computes
`PAGE_OFFSET` correctly when KASLR is disabled (it equals `VA(_text) - PA(_text)`
which equals `PAGE_OFFSET` in that case).

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASLR (Kernel Address Space Layout Randomization) on ARMv8-A is implemented by choosing a random physical load address (phys_offset) and a random virtual mapping offset (kimage_voffset) at boot time. The CPU does not know or care about randomization: it simply uses whatever address is in TTBR1_EL1 as the root of the kernel page table. The EL1 exception vector base (VBAR_EL1) is also randomized as a consequence of the kernel image being loaded at a random VA. The hardware has no KASLR-awareness.

### Kernel Perspective (Linux ARM64)
KASLR is implemented in __pi_kaslr_early_init (arch/arm64/kernel/pi/kaslr_early.c). It uses the EFI random number generator (or RNDR instruction on ARMv8.5+) to pick a random offset that is aligned to the minimum KASLR granularity (2 MB for 4 KB pages). The offset is applied by:
  1. Choosing a random VA base for the kernel image within the TTBR1_EL1 region.
  2. Updating kimage_voffset = VA - PA.
  3. Updating all ELF RELA relocation entries to reflect the new VA.
  4. Flushing the D-cache for all modified sections.

### Memory Perspective (ARMv8 Memory Model)
KASLR changes the kernel's VA layout but not its PA layout (for a given boot). The TTBR1_EL1 page tables point to the randomized kernel VA. kimage_voffset is a compile-time-unknown runtime constant used to convert between kernel PA and kernel VA: VA = PA + kimage_voffset. Because the kernel uses position-independent code (__pi_ prefix) during the relocation phase, all accesses are PA-relative until the relocations are applied. After __primary_switch, all kernel code runs at the final randomized VA.