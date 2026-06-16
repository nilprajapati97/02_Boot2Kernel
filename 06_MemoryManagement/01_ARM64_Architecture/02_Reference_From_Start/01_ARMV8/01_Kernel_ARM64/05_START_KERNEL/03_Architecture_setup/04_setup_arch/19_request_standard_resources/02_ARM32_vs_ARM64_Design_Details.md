# request_standard_resources() — ARM32 vs ARM64 Design Details

## 1. Both Architectures Have request_standard_resources()

Both ARM32 and ARM64 call `request_standard_resources()` in `setup_arch()`. The function serves the same purpose but differs in implementation details.

---

## 2. ARM32 Implementation

**File:** `arch/arm/kernel/setup.c`

```c
static void __init request_standard_resources(const struct machine_desc *mdesc)
{
    kernel_code.start = __pa(KERNEL_START);
    kernel_code.end   = __pa(__init_begin - 1);
    kernel_data.start = __pa(_sdata);
    kernel_data.end   = __pa(_end - 1);

    /* Allocate struct resource array for all RAM regions */
    res_size = sizeof(*res) * memblock.memory.cnt;
    res = memblock_alloc(res_size, SMP_CACHE_BYTES);

    for_each_mem_region(region) {
        res[i].name  = "System RAM";
        res[i].start = ...;
        res[i].end   = ...;
        res[i].flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
        request_resource(&iomem_resource, &res[i]);
        request_resource(&res[i], &kernel_code);
        request_resource(&res[i], &kernel_data);
    }

    /* Machine descriptor extras */
    if (mdesc->video_start) {
        video_ram.start = mdesc->video_start;
        video_ram.end   = mdesc->video_end;
        request_resource(&iomem_resource, &video_ram);
    }
}
```

ARM32 has `mdesc` pointer → can register video RAM from machine descriptor.

---

## 3. ARM64 Implementation

**File:** `arch/arm64/kernel/setup.c`

```c
static void __init request_standard_resources(void)
{
    struct memblock_region *region;
    struct resource *res;
    unsigned long i = 0;

    kernel_code.start = __pa_symbol(_text);
    kernel_code.end   = __pa_symbol(__init_begin) - 1;
    kernel_data.start = __pa_symbol(_sdata);
    kernel_data.end   = __pa_symbol(_end) - 1;

    for_each_mem_region(region) {
        res = alloc_bootmem_low(sizeof(*res));
        res->name  = "System RAM";
        res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
        res->end   = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
        res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
        request_resource(&iomem_resource, res);
        request_resource(res, &kernel_code);
        request_resource(res, &kernel_data);
    }
}
```

ARM64 key difference: uses `__pa_symbol()` instead of `__pa()` for KASLR compatibility.

---

## 4. __pa() vs __pa_symbol(): KASLR Difference

### ARM32: __pa()

```c
#define __pa(x) ((unsigned long)(x) - PAGE_OFFSET + PHYS_OFFSET)
```

Works when kernel is always loaded at `PHYS_OFFSET + (virt - PAGE_OFFSET)`. On ARM32, the kernel load address is fixed, so `__pa()` works for both data and code pointers.

### ARM64: __pa_symbol()

```c
/* For kernel symbols (known at link time), uses KASLR-aware conversion */
#define __pa_symbol(x) \
    __phys_addr_symbol(RELOC_HIDE((unsigned long)(x), 0))

/* Which expands to: */
static inline phys_addr_t __phys_addr_symbol(unsigned long x)
{
    /* x is a kernel symbol address (randomized by KASLR) */
    return x - kimage_voffset;
    /* kimage_voffset = kernel_VA_start - kernel_PA_start */
}
```

On ARM64 with KASLR, the kernel virtual address at each boot is different. `__pa()` uses `memstart_addr` (for data/stack) while `__pa_symbol()` uses `kimage_voffset` (for kernel code/data symbols). Using `__pa()` on a symbol address with KASLR active would give the wrong physical address.

---

## 5. resource_size_t Width

| | ARM32 | ARM64 |
|--|-------|-------|
| resource_size_t | 32-bit (or 64-bit with LPAE) | 64-bit |
| iomem_resource.end | 0xFFFFFFFF (4GB) | 0xFFFFFFFFFFFFFFFF (16 exabytes) |
| System RAM physical range | 32-bit (or 40-bit LPAE) | 48/52-bit |

ARM64 `struct resource` uses 64-bit physical addresses, allowing resources above 4GB. This is critical for ARM64 servers with DIMM slots above 4GB.

---

## 6. /proc/iomem: ARM32 vs ARM64 Examples

### ARM32 (Raspberry Pi 2 — Cortex-A7)

```
/proc/iomem:
00000000-3bffffff : System RAM
  00008000-006fffff : Kernel code
  00700000-00c0ffff : reserved (page tables)
  00c10000-01028fff : Kernel data
3c000000-3effffff : VideoCore (GPU RAM)  ← from mdesc->video_start
20000000-20ffffff : BCM2835 peripherals  ← added by device drivers
```

### ARM64 (Raspberry Pi 4 — Cortex-A72)

```
/proc/iomem:
000000000-003bffffff : System RAM
  000000000-0000fffff : (UEFI memory regions)
  000080000-02eaffff  : Kernel code
  02eb0000-032fffff   : Kernel data
004000000-0ffffffff : System RAM (second bank)
fc000000-ffffffff   : VideoCore (GPU)  ← added by vc4 driver
600000000-7ffffffff : System RAM (4GB+ DIMMs on Pi Compute Module)
```

ARM64's 64-bit addresses enable showing multi-bank high-memory RAM above 4GB.

---

## 7. Kernel Code vs Kernel Data Boundaries

Both architectures use linker script symbols to define code/data boundaries:

```c
/* Linker script symbols (arch/arm/kernel/vmlinux.lds.S) */
_text     → start of kernel code (.text section)
_etext    → end of kernel code
__init_begin → start of .init section (code that runs once during boot)
__init_end   → end of .init section (freed after boot)
_sdata    → start of kernel data
_edata    → end of initialized data
__bss_start → start of BSS (zero-initialized data)
_end      → end of BSS (end of kernel image)

ARM32:
  kernel_code: [__pa(_text) .. __pa(__init_begin - 1)]
  kernel_data: [__pa(_sdata) .. __pa(_end - 1)]

ARM64: same symbols, but __pa_symbol() for address conversion
```

---

## 8. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Function signature | `(const struct machine_desc *mdesc)` | `(void)` |
| Physical address conversion | `__pa()` | `__pa_symbol()` |
| KASLR aware | No | Yes |
| Video RAM registration | Yes (mdesc->video_start) | No mdesc |
| resource_size_t width | 32-bit (64-bit with LPAE) | 64-bit |
| Parallel port resources | Possible (lp0/lp1/lp2) | No (no legacy ISA) |
| crashkernel resource | Yes (if CONFIG_KEXEC_CORE) | Yes |
| iomem_resource.end | 0xFFFFFFFF | 0xFFFFFFFFFFFFFFFF |
| ACPI system memory | No | Yes (from ACPI tables) |
