# reserve_crashkernel() — ARM32 vs ARM64 Design Details

## 1. Same Concept, Different Address Constraints

Both ARM32 and ARM64 call `reserve_crashkernel()` in `setup_arch()`. The function behavior is similar but physical address constraints differ significantly.

---

## 2. ARM32: Low Memory Constraint

ARM32 physical address space is 32-bit (or 40-bit with LPAE). The crash kernel reservation has strict constraints:

```c
/* arch/arm/kernel/setup.c */
#define CRASH_ALIGN         SZ_32M       /* 32MB alignment */
#define CRASH_ADDR_LOW_MAX  SZ_512M      /* prefer < 512MB */

crash_base = memblock_phys_alloc_range(crash_size,
                                       CRASH_ALIGN,
                                       0,               /* minimum */
                                       CRASH_ADDR_LOW_MAX); /* maximum */
```

Why < 512MB on ARM32?
- The capture kernel needs DMA-capable memory for disk I/O (writing crash dump)
- ARM32 ZONE_DMA is typically 0-512MB
- If crash kernel is > 512MB, storage drivers in the capture kernel may fail to DMA

If `CRASH_ADDR_LOW_MAX` allocation fails (too much memory already reserved below 512MB):

```c
if (!crash_base) {
    /* Try anywhere in memory */
    crash_base = memblock_phys_alloc(crash_size, CRASH_ALIGN);
}
```

---

## 3. ARM64: High Memory Support

ARM64 systems (especially servers) have many gigabytes of RAM. ARM64 supports two crash memory regions:

```c
/* arch/arm64/kernel/setup.c */
void __init reserve_crashkernel(void)
{
    /* crashkernel=X,low → reserve below 4GB (for 32-bit DMA devices) */
    /* crashkernel=X,high → reserve above 4GB (for crash dump memory) */

    if (crashk_res.end) {
        /* Already reserved (from ACPI or EFI memory map) */
        return;
    }

    /* Try to reserve high memory first (above 4GB) */
    reserve_crashkernel_generic(cmdline, crash_size, crash_base,
                                CRASH_ADDR_HIGH_MAX, true);

    /* Also reserve low memory for DMA in capture kernel */
    reserve_crashkernel_low();
}
```

ARM64 `crashkernel=` syntax extensions:

```bash
# ARM64 specific: 
crashkernel=512M,high   # 512MB above 4GB (good for large memory systems)
crashkernel=128M,low    # 128MB below 4GB (for DMA in capture kernel)

# Both together (recommended for ARM64 servers with >16GB RAM):
crashkernel=512M,high crashkernel=128M,low
```

---

## 4. CRASH_ADDR_HIGH_MAX on ARM64

```c
/* arch/arm64/include/asm/kexec.h */
#define CRASH_ADDR_LOW_MAX    SZ_4G    /* 4GB boundary */
#define CRASH_ADDR_HIGH_MAX   ULONG_MAX  /* anywhere in 64-bit space */
```

On ARM64 servers with 1TB RAM, the crash reservation can be anywhere — the capture kernel uses stage-2 page tables and can access any physical address.

---

## 5. crashk_res and crashk_low_res

ARM64 uses two resource entries:

```c
/* kernel/kexec_core.c */
struct resource crashk_res = {      /* high memory crash region */
    .name  = "Crash kernel",
    .flags = IORESOURCE_BUSY | IORESOURCE_SYSTEM_RAM,
    .desc  = IORES_DESC_CRASH_KERNEL,
};

struct resource crashk_low_res = {  /* low memory crash region (ARM64 only) */
    .name  = "Crash kernel (low)",
    .flags = IORESOURCE_BUSY | IORESOURCE_SYSTEM_RAM,
    .desc  = IORES_DESC_CRASH_KERNEL,
};
```

`/proc/iomem` on ARM64 server:

```
000000000-03ffffffff : System RAM
  ...
  100000000-11fffffff : Crash kernel (low)   ← DMA-capable (below 4GB)
200000000-27fffffff : Crash kernel           ← High memory (8GB+)
```

---

## 6. SMP Secondary CPU Parking on Panic

On SMP, when a panic occurs and kexec switches to capture kernel:

### ARM32

```c
/* machine_kexec() → smp_send_stop() */
smp_send_stop();   /* Sends IPI to all secondary CPUs */
/* Secondary CPUs receive IPI and park in cpu_park_loop() */
```

### ARM64

```c
/* arch/arm64/kernel/machine_kexec.c */
machine_kexec_mask_interrupts();
smp_send_stop();
/* Secondary CPUs park at crash-safe location */
```

ARM64 secondary CPUs park in a small spin-wait loop that's safe to execute even if the main kernel is corrupted.

---

## 7. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Max crash address (auto) | 512MB (DMA zone) | 4GB (low) or unlimited (high) |
| Alignment | 32MB | 2MB (page-aligned) |
| Two memory regions | No | Yes (low+high) |
| CRASH_ADDR_LOW_MAX | 512MB | 4GB |
| crashkernel=X,high | Not supported | Yes |
| crashkernel=X,low | Not applicable | Yes (for capture kernel DMA) |
| Max crash size practical | 128-256MB (out of 512MB-2GB total) | 1GB-4GB (out of 64GB+ total) |
| ACPI crash region | Not supported | Yes (ACPI SRAT can specify) |
| /proc/iomem entries | 1 (Crash kernel) | 1 or 2 (Crash kernel + low) |
