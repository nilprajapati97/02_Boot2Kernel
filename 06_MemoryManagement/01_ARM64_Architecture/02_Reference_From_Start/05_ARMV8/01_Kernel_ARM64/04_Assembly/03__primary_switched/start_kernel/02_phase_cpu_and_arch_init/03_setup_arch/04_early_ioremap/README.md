# Early I/O Remapping — `early_ioremap`

## The Problem

During early boot (`setup_arch()`), the kernel needs to read hardware tables in physical memory:
- ACPI tables (RSDP → RSDT/XSDT → MADT, SRAT, SLIT, DMAR)
- DMI/SMBIOS tables (hardware identity)
- EFI configuration tables
- The DTB itself (on ARM)

But `ioremap()` — the normal way to map physical addresses to virtual — requires `vmalloc()` area, which requires the page allocator, which isn't set up yet.

---

## Solution: `early_ioremap`

`early_ioremap` is a **compile-time fixed-address** temporary mapping mechanism:

```c
// arch/x86/mm/ioremap.c
void __init *early_ioremap(resource_size_t phys_addr, unsigned long size)
{
    // Uses pre-allocated fixmap slots (fixed virtual addresses)
    // Maps the physical address into a fixmap slot
    // Returns the virtual address
}

void __init early_iounmap(void *addr, unsigned long size)
{
    // Removes the fixmap mapping
}
```

### Fixmap Slots

```c
// arch/x86/include/asm/fixmap.h
enum fixed_addresses {
    // Early ioremap slots:
    FIX_EARLYCON_MEM_BASE,
    FIX_BTMAP_END,
    FIX_BTMAP_BEGIN = FIX_BTMAP_END + NR_FIX_BTMAPS * FIX_BTMAPS_SLOTS - 1,
    // ...
};
```

These are virtual addresses baked into the kernel at compile time. They are valid from the moment the kernel's page tables are set up (which happens in head_64.S before `start_kernel()`).

---

## Usage Example

```c
// Reading ACPI RSDP at physical address 0x000F0000
void *rsdp = early_ioremap(0x000F0000, PAGE_SIZE);
if (memcmp(rsdp, "RSD PTR ", 8) == 0) {
    // Valid RSDP found, parse ACPI tables
}
early_iounmap(rsdp, PAGE_SIZE);
```

---

## Transition to Regular ioremap

After `mm_core_init()` (Phase 4), `vmalloc` is available, and `ioremap()` can be used normally. `early_ioremap_reset()` is called to mark the early ioremap system as inactive — further `early_ioremap()` calls would panic.

---

## Interview Q&A

### Q1: What are fixmap addresses and why are they safe to use before paging is fully set up?
**A:** Fixmap addresses are virtual addresses whose page table entries are pre-populated in the kernel's initial page tables (set up in assembly before `start_kernel()`). They are in the kernel's virtual address space but point to configurable physical addresses. The kernel modifies only the physical address they point to — the PTE slot is always valid. This is safe from the moment paging is enabled, unlike `vmalloc` addresses which require dynamic PTE allocation.

### Q2: How does `early_ioremap` handle cache attributes?
**A:** Physical hardware registers must be mapped as **uncacheable** (UC) to ensure every read/write goes directly to hardware. `early_ioremap` uses the `pgprot_noncached()` protection, which on x86 sets the PTE's PAT/PCD/PWT bits to select the UC memory type in the CPU's Memory Type Range Registers (MTRRs). This prevents the CPU from caching MMIO register values, which would cause stale reads of hardware state.
