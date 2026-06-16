# reserve_crashkernel() — Detailed Design Bottom-To-Top Flow

## 1. What Is kdump?

**kdump** is a Linux kernel crash dump mechanism. When the production kernel panics (kernel bug, hardware fault, security exploit), kdump starts a second, pre-loaded **capture kernel** that saves the crashed kernel's memory to disk for post-mortem debugging.

Without kdump:
- Kernel panic → printk message → system hangs or reboots
- Memory contents (stack traces, data structures) lost forever
- No way to analyze root cause

With kdump:
- Kernel panic → kexec into capture kernel → capture kernel reads `/dev/crash` → writes crash dump to `/var/crash/` → analysis with `crash` tool

---

## 2. reserve_crashkernel(): Reserving Memory for the Capture Kernel

For kdump to work, physical memory must be reserved **at boot time** for the capture kernel. This memory must be:
1. **Reserved**: Production kernel's buddy allocator cannot use it
2. **Contiguous**: Capture kernel needs to load cleanly
3. **Fixed address**: `crashk_res.start` is known at boot, so the production kernel can jump there on panic

```c
/* arch/arm/kernel/setup.c */
static void __init reserve_crashkernel(void)
{
    unsigned long long crash_size, crash_base;
    int ret;

    /* Parse "crashkernel=SIZE[@BASE]" from kernel cmdline */
    ret = parse_crashkernel(boot_command_line, memblock_phys_mem_size(),
                            &crash_size, &crash_base);
    if (ret || !crash_size)
        return;

    crash_size = PAGE_ALIGN(crash_size);

    if (crash_base <= 0) {
        /* Auto-select: find suitable contiguous region */
        crash_base = memblock_phys_alloc_range(crash_size,
                                               CRASH_ALIGN, CRASH_ADDR_LOW_MAX);
        if (!crash_base) {
            pr_warn("crashkernel reservation failed - ...\n");
            return;
        }
    } else {
        /* User specified fixed address */
        if (memblock_reserve(crash_base, crash_size)) {
            pr_warn("crashkernel=size@base: failed to reserve memory\n");
            return;
        }
    }

    crashk_res.start = crash_base;
    crashk_res.end   = crash_base + crash_size - 1;
    insert_resource(&iomem_resource, &crashk_res);
    pr_info("Reserving %ldMB of memory at %ldMB for crashkernel\n",
            (unsigned long)(crash_size >> 20),
            (unsigned long)(crash_base >> 20));
}
```

---

## 3. crashk_res: The Crash Kernel Resource

```c
/* kernel/kexec_core.c */
struct resource crashk_res = {
    .name  = "Crash kernel",
    .start = 0,
    .end   = 0,
    .flags = IORESOURCE_BUSY | IORESOURCE_SYSTEM_RAM,
    .desc  = IORES_DESC_CRASH_KERNEL,
};
```

After `reserve_crashkernel()`:
- `crashk_res.start` and `.end` contain the physical address range
- This resource is inserted into the `iomem_resource` tree
- `/proc/iomem` shows: `01800000-01ffffff : Crash kernel`
- `memblock` has this region marked as "reserved"

---

## 4. Kernel Command Line: crashkernel= Syntax

```bash
# Fixed size at automatic address
crashkernel=128M

# Fixed size at fixed physical address
crashkernel=128M@256M    # 128MB at 256MB physical

# Ranged spec: different sizes based on total RAM
crashkernel=512M-2G:128M,2G-:256M
# If total RAM is 512MB-2GB, reserve 128MB
# If total RAM is >2GB, reserve 256MB

# For high memory (ARM64 servers)
crashkernel=1G,high   # Reserve 1GB above 4GB (ZONE_DMA32 excluded)
```

---

## 5. kexec and the Crash Dump Flow

```
Production kernel (normal operation):
  [ reserve_crashkernel() at boot ]
  [ load_crashkernel_image() via kexec tool at runtime ]
    → capture kernel + initramfs loaded into reserved memory
    → kexec boot parameters set up

Production kernel PANIC:
  panic()
    → __crash_kexec()
      → machine_kexec(image)
        → disable SMP (send IPI to park secondary CPUs)
        → disable IRQs
        → flush cache
        → jump to capture kernel entry point

Capture kernel:
  Boots from scratch (fresh kernel, minimal initramfs)
  Maps production kernel's /dev/crash (physical memory as flat file)
  makedumpfile writes crash dump
  System reboots normally
```

---

## 6. CRASH_ALIGN and CRASH_ADDR_LOW_MAX

```c
/* arch/arm/kernel/setup.c */
#define CRASH_ALIGN     SZ_32M    /* 32MB alignment */
#define CRASH_ADDR_LOW_MAX  SZ_512M  /* ARM32: prefer < 512MB (DMA zone) */
```

The crash kernel is preferably allocated below 512MB on ARM32 to keep it in the DMA zone, ensuring DMA devices work in the capture kernel without special setup.

---

## 7. Interview Q&A

**Q1: Why must crashkernel memory be reserved at boot? Why not allocate it dynamically when a panic occurs?**
> During a kernel panic, the system is in an unstable state — the allocator itself may be corrupted (the bug that caused the panic may have overwritten allocator data structures). `kexec` needs to jump to a clean piece of memory with a valid capture kernel already loaded. By the time panic() fires, there's no opportunity to safely call `memblock_alloc()` or even `kmalloc()`. The reserved memory is guaranteed to be untouched by the production kernel's normal operation throughout the system lifetime. This is the fundamental constraint of crash dump: reservation must be prophylactic (at boot), not reactive (at panic).

**Q2: What happens if crashkernel memory is not reserved (no cmdline option)?**
> `reserve_crashkernel()` calls `parse_crashkernel(boot_command_line, ...)` which returns `-EINVAL` if no `crashkernel=` option is present. The function returns early, `crashk_res.start` remains 0, and `crashk_res.end` remains 0. `/proc/iomem` shows no "Crash kernel" entry. The `kexec -p` command (load capture kernel) will fail with "No crashkernel" error. Kernel panics will reboot normally without saving a dump. For production servers where crash analysis is important, `crashkernel=` should always be in the kernel cmdline.

**Q3: How does the capture kernel know where the crashed kernel's memory is?**
> When the production kernel crashes and jumps to the capture kernel, it passes a modified E820/memmap describing what is the "current memory" from the capture kernel's perspective. The physical addresses where the production kernel's RAM lived are accessible to the capture kernel as `/dev/crash` — a special device driver that exposes all physical memory as a flat byte array. The capture kernel also has access to the `vmcoreinfo` section (exported by the production kernel via a known physical address) which contains the offset of `swapper_pg_dir`, `init_task`, and other symbols needed by the `crash` analysis tool to reconstruct the kernel data structures.
