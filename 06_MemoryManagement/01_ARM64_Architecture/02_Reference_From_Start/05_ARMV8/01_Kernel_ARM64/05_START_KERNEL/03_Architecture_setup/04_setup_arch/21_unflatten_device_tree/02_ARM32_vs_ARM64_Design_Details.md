# unflatten_device_tree() — ARM32 vs ARM64 Design Details

## 1. Same Function, Different Context

`unflatten_device_tree()` itself is in `drivers/of/fdt.c` and is **architecture-independent**. Both ARM32 and ARM64 call it. Differences are in:
- When it's called in the boot sequence
- Memory allocator used
- FDT discovery method
- Multi-board vs single firmware-defined system

---

## 2. FDT Discovery: ARM32 vs ARM64

### ARM32: FDT from Bootloader Register

```c
/* arch/arm/kernel/setup.c — early in setup_arch() */
mdesc = setup_machine_fdt(__fdt_pointer);
```

`__fdt_pointer` is set from register `r2` (the FDT physical address passed by U-Boot). The FDT pointer is discovered at the very start of boot.

```c
/* arch/arm/kernel/head-common.S */
/* r2 = FDT blob address (from U-Boot/bootloader) */
str    r2, [ip, #4]          /* save to __fdt_pointer */
```

ARM32 can also boot from ATAGs (legacy) — if `r2` points to ATAGs instead of FDT, a stub DT is created.

### ARM64: FDT from x1 Register

```c
/* arch/arm64/kernel/head.S */
/* x1 = physical address of FDT blob (from firmware/EFI) */
adrp    x23, __fdt_pointer
str     x1, [x23, :lo12:__fdt_pointer]
```

ARM64 systems using UEFI firmware: The UEFI stub saves the FDT address in a UEFI config table. The kernel's EFI stub retrieves it and passes it in x1.

---

## 3. Memory Allocator During Unflattening

Both architectures use `memblock_alloc()` for the tree memory, but the specific function differs:

### ARM32

```c
/* arch/arm/mm/init.c */
void * __init early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
    return memblock_alloc(size, align);
}
```

Returns a virtual address (kernel direct-mapped memory).

### ARM64

```c
/* arch/arm64/mm/init.c */
void * __init early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
    void *ptr = memblock_alloc(size, align);
    if (!ptr)
        panic("Early DT alloc failed: size=%llu align=%llu\n", size, align);
    return ptr;
}
```

Same mechanism, with explicit panic on failure (stricter on ARM64).

---

## 4. Device Tree Usage After Unflattening

### ARM32 (after unflatten_device_tree())

Immediately used by:
1. `arm_dt_init_cpu_maps()` — reads `/cpus` node for `cpu_logical_map[]`
2. `psci_dt_init()` — reads `cpus/cpu@0/enable-method` = "psci"
3. `smp_init_cpus()` — reads CPU nodes to register secondary CPUs

### ARM64 (after unflatten_device_tree())

```c
/* arch/arm64/kernel/setup.c */
unflatten_device_tree();
psci_dt_init();            ← reads DT for PSCI method
xen_early_init();          ← reads DT for Xen hypervisor
efi_init();                ← reads DT for EFI memory map
...
```

ARM64 has more firmware layers (UEFI, Xen, PSCI) that all probe the DT immediately after unflattening.

---

## 5. DT Overlays: ARM32 (rare) vs ARM64 (Raspberry Pi 4)

ARM64 Raspberry Pi uses **DT overlays** — the bootloader can apply runtime patches to the base DT:

```
base DT: bcm2711-rpi-4-b.dtb
overlays: overlays/vc4-kms-v3d-pi4.dtbo
          overlays/disable-bt.dtbo
```

DT overlays use `of_overlay_fdt_apply()` which modifies the live `device_node` tree after `unflatten_device_tree()`. ARM32 systems rarely use overlays (they use different board DTs).

---

## 6. ACPI vs DT: ARM64 Server Consideration

On ARM64 servers (ThunderX, Ampere Altra), the system may boot with ACPI instead of DT:

```c
/* arch/arm64/kernel/setup.c */
if (acpi_disabled)
    unflatten_device_tree();
else
    acpi_init_numa_mappings();
```

ARM64 with ACPI does NOT call `unflatten_device_tree()`. ACPI provides hardware description via DSDT/SSDT tables. `of_root` remains NULL. All `of_find_*()` calls return NULL — drivers must use `acpi_find_*()` instead. ARM32 always uses DT (ACPI on ARM32 does not exist).

---

## 7. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| FDT source | Register r2 from bootloader | Register x1 or UEFI config table |
| ATAG fallback | Yes (legacy ARM32 boards) | No |
| Function called | `unflatten_device_tree()` (always) | Only if `acpi_disabled` |
| ACPI alternative | Not available | Yes (for server-class ARM64) |
| DT Overlays | Rare | Common (Raspberry Pi) |
| of_root after unflat | Set to DT root node | Set to DT root or NULL (ACPI) |
| Allocator | `memblock_alloc()` | `memblock_alloc()` (with panic check) |
| Memory usage | 300KB-1MB typical | 1-5MB (more devices on servers) |
