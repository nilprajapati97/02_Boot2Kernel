# request_standard_resources() — Detailed Design

## 1. What Is the Resource Tree?

The Linux kernel maintains a **global resource tree** rooted at `iomem_resource` and `ioport_resource`. This tree tracks all I/O memory and port address ranges claimed by hardware and the kernel:

```
iomem_resource (root: 0 - 0xFFFFFFFF)
  ├── System RAM [0x00000000 - 0x0FFFFFFF]
  │     ├── Kernel code [0xC0008000 - 0xC0500000]
  │     └── Kernel data [0xC0500000 - 0xC1000000]
  ├── GPU reserved [0x10000000 - 0x11FFFFFF]
  ├── Device MMIO [0x20000000 - 0x2FFFFFFF]
  └── (more regions...)
```

The resource tree serves as:
1. **Conflict detection**: Two drivers can't claim the same MMIO range
2. **Information export**: `/proc/iomem` reads this tree
3. **Debugging**: Shows what physical memory ranges are used for what

---

## 2. Source Code Analysis

**File:** `arch/arm/kernel/setup.c`

```c
static void __init request_standard_resources(const struct machine_desc *mdesc)
{
    struct memblock_region *region;
    struct resource *res;
    unsigned long i = 0;
    size_t res_size;

    kernel_code.start   = __pa(KERNEL_START);
    kernel_code.end     = __pa(__init_begin - 1);
    kernel_data.start   = __pa(_sdata);
    kernel_data.end     = __pa(_end - 1);
```

The kernel_code and kernel_data resources are defined as static global `struct resource`:

```c
static struct resource kernel_code = {
    .name  = "Kernel code",
    .flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
};
static struct resource kernel_data = {
    .name  = "Kernel data",
    .flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
};
```

By setting `.start` and `.end` here (using physical addresses), `request_standard_resources()` fills in the runtime-determined extents of the kernel image.

```c
    res_size = sizeof(*res) * memblock.memory.cnt;
    res = memblock_alloc(res_size, SMP_CACHE_BYTES);
    if (!res)
        panic(...);
```

One `struct resource` is allocated for each memblock memory region (each contiguous RAM block). On most systems, there's 1-3 memory blocks.

```c
    for_each_mem_region(region) {
        res[i].name  = "System RAM";
        res[i].start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
        res[i].end   = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
        res[i].flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
        request_resource(&iomem_resource, &res[i]);
```

Each contiguous physical RAM region becomes a "System RAM" resource in the iomem tree.

```c
        if (kernel_code.start >= res[i].start &&
            kernel_code.end   <= res[i].end)
            request_resource(&res[i], &kernel_code);
        if (kernel_data.start >= res[i].start &&
            kernel_data.end   <= res[i].end)
            request_resource(&res[i], &kernel_data);
```

The kernel code and data resources are registered as **children** of the System RAM resource that contains them. This creates the tree hierarchy shown above.

```c
#ifdef CONFIG_KEXEC_CORE
        /* If crashkernel reservation is in this RAM, register it */
        request_resource(&res[i], &crashk_res);
#endif
        i++;
    }
}
```

---

## 3. The iomem_resource Tree After request_standard_resources()

On a typical ARM32 system with 512MB RAM:

```
/proc/iomem (after request_standard_resources):
00000000-1fffffff : System RAM
  00008000-00bfffff : Kernel code
  00c00000-01ffffff : reserved (kernel data starts later)
  ...
  00500000-01000000 : Kernel data
20000000-2fffffff : (device MMIO — added later by drivers)
...
```

The tree structure allows the kernel to check: "Is address X claimed? By what?" Using `request_resource()` for overlapping ranges returns `-EBUSY`, preventing conflicts.

---

## 4. struct resource Definition

```c
struct resource {
    resource_size_t start;    /* physical start address */
    resource_size_t end;      /* physical end address (inclusive) */
    const char *name;         /* human-readable name for /proc/iomem */
    unsigned long flags;      /* IORESOURCE_IO, IORESOURCE_MEM, etc. */
    unsigned long desc;       /* IORES_DESC_NONE, IORES_DESC_CRASH_KERNEL, etc. */
    struct resource *parent;  /* parent in tree */
    struct resource *sibling; /* next sibling */
    struct resource *child;   /* first child */
};
```

---

## 5. Other Standard Resources on ARM32

Some boards also register:
- **Video memory**: `mdesc->video_start / video_end` → "Video RAM area"
- **Parallel port**: lp0, lp1, lp2 at legacy I/O addresses (ARM32 rarely has these)
- **ROM/flash**: If the machine has ROM at fixed address

The "standard" in `request_standard_resources()` refers to the common set every ARM32 system should have: System RAM, Kernel code, Kernel data.

---

## 6. /proc/iomem and /proc/ioports

These proc files enumerate the resource tree:

```bash
$ cat /proc/iomem
00000000-1fffffff : System RAM
  00008000-00bfffff : Kernel code
  00c00000-021fffff : Kernel data
40000000-5fffffff : System RAM (second bank, if present)
```

```bash
$ cat /proc/ioports   (ARM: usually empty — no legacy ISA I/O ports)
```

These files are read by:
- `lshw` / `hwinfo` (hardware inventory tools)
- `kdump` / `makedumpfile` (crash dump — need to know where kernel is)
- Memory ECC tools (need to know which addresses are RAM vs MMIO)

---

## 7. Call Tree (Bottom-Up)

```
request_resource()           ← kernel/resource.c
memblock_alloc()             ← mm/memblock.c
        ▲
request_standard_resources(mdesc)  ← arch/arm/kernel/setup.c
        ▲
setup_arch()                 ← arch/arm/kernel/setup.c (after kasan_init)
```

---

## 8. Interview Q&A

**Q1: What is the difference between memblock and the resource tree?**
> `memblock` is an early boot allocator — it tracks physical memory regions as either "memory" (exists) or "reserved" (in use). It's used during boot before the buddy allocator is ready and is largely replaced by the buddy allocator after `mem_init()`. The **resource tree** is a persistent data structure for the kernel's entire lifetime. It records which physical address ranges are claimed by what (System RAM, device MMIO, kernel code, etc.). The resource tree is used for conflict detection (`request_resource()` fails if a range is claimed), `/proc/iomem` output, and device driver MMIO registration (`request_mem_region()` before `ioremap()`).

**Q2: Why are kernel_code and kernel_data registered after paging_init() rather than earlier?**
> `request_standard_resources()` uses `memblock_alloc()` to allocate the `struct resource` array for System RAM regions. `memblock_alloc()` uses physical memory — it needs the permanent page tables (from `paging_init()`) to be in place because the allocated memory needs to be accessible via its kernel virtual address. Additionally, `kernel_code.start = __pa(KERNEL_START)` is valid throughout, but allocating the containing `res[]` array requires post-paging-init memblock. There's no strict technical requirement to wait, but `request_standard_resources()` is naturally ordered here since it depends on `paging_init()` completing first.

**Q3: What happens if two drivers both call request_mem_region() for the same physical address?**
> `request_mem_region()` calls `request_resource(&iomem_resource, res)` internally. `request_resource()` traverses the resource tree and returns `-EBUSY` if the requested range overlaps any already-claimed child resource. The second driver's `request_mem_region()` call returns NULL. A well-written driver checks this return value and fails the probe, printing a message like "can't get I/O memory space". This prevents two drivers from fighting over the same hardware register — a common bug in embedded systems where device tree is wrong and two different drivers claim the same MMIO base address.

**Q4: Why does request_standard_resources() use IORESOURCE_BUSY flag?**
> `IORESOURCE_BUSY` means this resource is actively in use and should not be allocated by `request_resource()` to any other user. For "System RAM" resources, this flag ensures that no driver can accidentally claim a system RAM region as its MMIO. For kernel code/data, BUSY indicates the kernel itself is using this memory. Some resources are allocated without BUSY (informational only — they appear in /proc/iomem but don't block other allocations). `IORESOURCE_BUSY` is the difference between "allocated and exclusive" vs "informational".
