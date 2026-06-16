# request_standard_resources() — System Design Approach and Q&A

## 1. Why the Resource Tree Exists: The MMIO Conflict Problem

Physical address space is shared between RAM, device MMIO, and firmware regions. Without a conflict-detection mechanism:
- Two drivers could claim the same MMIO region
- Driver A maps UART at 0x20000000, Driver B maps I2C at 0x20000000 (due to wrong DT)
- Both call `ioremap(0x20000000, ...)` — both get a VA pointing to the same hardware
- Writing to "I2C" accidentally configures "UART" — silent hardware bug

The resource tree provides **serialized, conflict-detecting registration** of all physical address ranges.

---

## 2. Design Principle: Fail-Fast Conflict Detection

```c
int request_resource(struct resource *root, struct resource *new)
{
    struct resource *conflict = __request_resource(root, new);
    if (!conflict)
        return 0;
    return -EBUSY;
}
```

If `new->start` – `new->end` overlaps any existing child of `root`, returns `-EBUSY`. This is fail-fast: the conflict is detected at registration time (probe), not at runtime (first MMIO access). The driver's `probe()` function gets an error and can exit cleanly rather than corrupting hardware state.

---

## 3. Dependency Graph

```
[paging_init()]
  └── struct page array allocated (memblock_alloc works properly)
        │
[kasan_init()]
  └── shadow mapped
        │
[request_standard_resources(mdesc)]
  ├── __pa(KERNEL_START) → kernel_code.start
  ├── for_each_mem_region → "System RAM" resources
  ├── request_resource(&iomem_resource, ...) → tree populated
  └── /proc/iomem becomes readable
        │
[Driver init (later — after start_kernel → rest_init → init)]
  ├── device driver probe() → request_mem_region() → request_resource()
  │     Conflicts with System RAM detected immediately
  ├── PCI bus scan → pci_request_resource() for BAR regions
  └── ACPI tables → ACPI_ADDR_SPACE_MEMORY resources registered
```

---

## 4. The resource Tree vs iomem_resource

```
iomem_resource (the root, covers all physical address space):
  ├── "System RAM" [00000000-1FFFFFFF]
  │     ├── "Kernel code"  [00008000-006FFFFF]
  │     ├── "Kernel data"  [00C10000-01028FFF]
  │     └── "Crash kernel" [01800000-01FFFFFF] (if reserve_crashkernel)
  ├── "Board MMIO" [20000000-20FFFFFF]   ← from mdesc->map_io or drivers
  ├── "PCI MMIO"  [40000000-4FFFFFFF]   ← from PCI bus driver
  └── "GPU reserved" [30000000-31FFFFFF] ← from mdesc->reserve / DT

ioport_resource (for x86 I/O ports — empty on most ARM systems):
  (empty on ARM — no legacy ISA I/O port space)
```

The hierarchical structure means: a child resource must be entirely within its parent's range. "Kernel code" is a child of "System RAM" because kernel code is physically located within system RAM.

---

## 5. Security Consideration: Preventing /dev/mem Exploitation

`/dev/mem` allows user space processes to read/write arbitrary physical memory. This is a security risk (can read crypto keys from RAM, modify kernel data). The resource tree is used to restrict `/dev/mem`:

```c
/* drivers/char/mem.c */
static int devmem_is_allowed(unsigned long pfn)
{
    if (iomem_is_exclusive(pfn << PAGE_SHIFT)) {
        /* IORESOURCE_BUSY regions with IORESOURCE_EXCLUSIVE flag */
        return 0;   /* access denied */
    }
    ...
}
```

Resources registered with `IORESOURCE_EXCLUSIVE` (which `IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY` implies on modern kernels) block `/dev/mem` access to those physical addresses. This prevents user space from reading kernel memory via `/dev/mem` on systems with `CONFIG_STRICT_DEVMEM`.

---

## 6. System Design Q&A

**Q: How does the resource tree handle the case where kernel memory spans two memblock regions?**
> `kernel_code` is registered as a child of the System RAM resource that contains its physical address range. `request_standard_resources()` checks: `if (kernel_code.start >= res[i].start && kernel_code.end <= res[i].end)`. If the kernel image somehow spanned two memblock regions (extremely unusual — would require a memory hole in the middle of the kernel), `request_resource()` for `kernel_code` would fail for the second region's `res[i]` because `kernel_code.end` would exceed `res[i].end`. In practice, bootloaders always load the kernel within a single contiguous memblock region.

**Q: What is insert_resource() vs request_resource() and when is each used?**
> `request_resource()` adds a resource as a child, but requires that no existing child overlaps the new resource — returns `-EBUSY` on conflict. `insert_resource()` adds a resource and **reorganizes** the tree if needed: if the new resource overlaps existing children, those children become children of the new resource (the new resource "absorbs" them as children). `insert_resource()` is used for "parent" regions discovered after their children were already registered. For example, `reserve_crashkernel()` uses `insert_resource()` to add the crash kernel region into "System RAM" even though "Kernel code" and "Kernel data" might already be registered.

**Q: Why is it important to register System RAM in the iomem resource tree? Isn't it obvious what's RAM?**
> It's not obvious to the kernel subsystems. Without explicit registration: (1) `/proc/iomem` would show no RAM entries — users/tools couldn't determine which physical addresses are RAM. (2) `request_mem_region()` for MMIO at a RAM address would succeed silently — a driver bug that maps RAM as MMIO would go undetected. (3) `kdump`/`makedumpfile` uses `/proc/iomem` to find RAM regions for crash dumps — no System RAM entries means crash dumps don't work. (4) EFI memory map tools cross-reference UEFI-declared RAM with Linux's iomem tree for integrity checking.

**Q: How does request_standard_resources() interact with NUMA on multi-NUMA ARM64 servers?**
> On NUMA systems, each NUMA node has its own physical RAM banks. `for_each_mem_region()` returns all physical RAM regions including multi-NUMA banks. Each bank gets its own "System RAM" entry in the iomem tree. The NUMA node association comes from memblock's `MEMBLOCK_FLAGS_HOTPLUG` flags and nid fields. `request_resource()` itself doesn't know about NUMA — it just builds the address-range tree. NUMA-aware allocation is handled by the zone/node structures built by `free_area_init()`, not the resource tree.
