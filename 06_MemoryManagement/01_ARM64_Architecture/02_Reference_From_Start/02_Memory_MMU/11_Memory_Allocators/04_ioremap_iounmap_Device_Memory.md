# ioremap, iounmap, and Device Memory Access

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
ioremap: map device MMIO (Memory-Mapped I/O) registers into kernel virtual address space

  Problem: device registers live at physical addresses assigned by:
    - System firmware (UEFI/ACPI resource tables)
    - Device tree (FDT compatible nodes with reg property)
    - PCIe BAR (Base Address Registers)
  
  These physical addresses cannot be accessed via __va() or normal pointers:
    __va() only works for RAM (linear-mapped physical memory)
    MMIO addresses may not be in RAM at all
    MMIO requires DEVICE memory attributes (no caching, strict ordering)
  
  ioremap solution:
    Allocate a range in the vmalloc virtual address space
    Create page table entries mapping that VA range → device PA
    Use Device memory attributes in PTEs (not Normal memory!)
    Return pointer to caller — driver reads/writes registers through this pointer
  
  ARM64 vs x86 difference:
    x86: needs explicit IN/OUT instructions for port I/O, OR ioremap for MMIO
    ARM64: ALL device access is memory-mapped (no separate port I/O)
           ioremap is the ONLY way to access hardware registers on ARM64
           This makes ARM64 driver code cleaner

Common ioremap variants:
  ioremap(pa, size):          nGnRE (Device non-Gathering, non-Reordering, Early-Ack)
  ioremap_nocache(pa, size):  same as ioremap (DEFAULT for most MMIO)
  ioremap_wc(pa, size):       Write-Combining (normal memory, weakly ordered)
                               ARM64: Normal Non-Cacheable or Device GRE
                               Use for: frame buffers, large bulk writes
  ioremap_uc(pa, size):       Uncacheable (strongest ordering)
                               ARM64: Device nGnRnE (strictest)
  devm_ioremap(dev, pa, sz):  automatic cleanup on device removal
  devm_ioremap_resource():    maps device tree / ACPI resource + ioremap
```

---

## 2. ARM64 MAIR and Device Memory Types

```
ARM64 Memory Attributes (MAIR_EL1 encoding for ioremap):

Device memory types (bits [1:0] of the attribute byte):
  0b0000: Device nGnRnE  (non-Gathering, non-Reordering, no Early write Ack)
          Strictest: no merging, no reordering, write acknowledged at device
          Use: ordered legacy devices
          ioremap_uc() uses this (or nGnRE, platform-specific)
  
  0b0100: Device nGnRE   (non-Gathering, non-Reordering, Early write Ack)
          No merging, no reordering, but CPU doesn't wait for device to ACK write
          Default ioremap() on ARM64 for most MMIO
          Use: most device registers
          Linux default: MAIR index = MT_DEVICE_nGnRE
  
  0b1000: Device nGRE    (non-Gathering, Reordering, Early write Ack)
          No merging, but reordering allowed
          Rare in practice
  
  0b1100: Device GRE     (Gathering, Reordering, Early write Ack)
          Writes can be merged; reordering allowed
          Used for: some WC-like device regions
  
  For ioremap_wc (write-combining):
    ARM64: Normal Non-Cacheable: 0b01000100 (Outer NC, Inner NC)
           OR: used by ioremap_wc in Linux for WC/UC non-device regions
           Use: frame buffers (CPU writes large blocks, hardware reads later)
           Allows CPU to buffer writes (write-combining buffer) → better throughput

MAIR_EL1 default Linux setup (4 used indices):
  Index 0 (MT_DEVICE_nGnRnE): 0x00  (Device nGnRnE)
  Index 1 (MT_DEVICE_nGnRE):  0x04  (Device nGnRE)    ← default ioremap
  Index 2 (MT_DEVICE_GRE):    0x0C  (Device GRE)
  Index 3 (MT_NORMAL_NC):     0x44  (Normal NC)
  Index 4 (MT_NORMAL):        0xFF  (Normal WB+WA)     ← RAM
  Index 5 (MT_NORMAL_TAGGED): 0xF0  (Normal with MTE tagging)

PTE for ioremap (AttrIndx field bits [4:2]):
  AttrIndx = 1 → MAIR index 1 = MT_DEVICE_nGnRE
  Shareability: SH=00 (non-shareable, not inner-shareable like RAM)
                (Device memory is inherently shareable at the device level)
  AP: EL1 only (kernel), read-write
  PXN=0 (kernel can read), UXN=1 (user cannot execute/access)
```

---

## 3. ioremap Implementation

```c
/* arch/arm64/mm/ioremap.c */

void __iomem *ioremap(phys_addr_t phys_addr, size_t size)
{
    return __ioremap_caller(phys_addr, size, __pgprot(PROT_DEVICE_nGnRE),
                            __builtin_return_address(0));
}

void __iomem *__ioremap_caller(phys_addr_t phys_addr, size_t size,
                                pgprot_t prot, void *caller)
{
    // 1. Validate: not RAM (MMIO should not be in known RAM regions)
    //    memblock_is_memory(phys_addr) should be false
    
    // 2. Align to page boundary:
    offset = phys_addr & (~PAGE_MASK);   // offset within page
    phys_addr &= PAGE_MASK;              // align down to page
    size = PAGE_ALIGN(size + offset);    // align up
    
    // 3. Allocate VM area in vmalloc space:
    area = get_vm_area_caller(size, VM_IOREMAP, caller);
    area->phys_addr = phys_addr;
    
    // 4. Map device physical range into vmalloc VA:
    ioremap_page_range(area->addr, area->addr + size, phys_addr, prot):
        // For each page:
        pud = pud_alloc(&init_mm, pgd, addr);
        pmd = pmd_alloc(&init_mm, pud, addr);
        pte = pte_alloc_kernel(pmd, addr);
        // Set PTE with device memory attributes:
        // PTE = [phys_addr[47:12] | AttrIndx | AP_EL1_RW | PXN=1 | UXN=1 | valid]
        set_pte_at(&init_mm, addr, pte, pfn_pte(pfn, prot));
    
    // 5. Flush TLB: ensure no stale entries for this VA range
    //    (new mappings: not strictly needed if VA was never mapped before,
    //    but done for safety)
    
    // 6. Return VA + original page offset:
    return (__force void __iomem *)(area->addr + offset);
}
```

---

## 4. iounmap Implementation

```c
void iounmap(volatile void __iomem *io_addr)
{
    addr = (void *)((unsigned long)io_addr & PAGE_MASK);
    area = find_vm_area(addr);
    
    // 1. Remove the mapping (clear PTEs):
    remove_vm_area(addr);
    // Walks page tables, clears each PTE, frees empty table pages
    
    // 2. TLB flush: CRITICAL for device memory!
    //   Must ensure no CPU has stale TLB entry mapping to old MMIO PA
    //   After iounmap, another device may be mapped to same PA
    //   Accessing through stale TLB would hit wrong device!
    //   ARM64: flush_tlb_kernel_range(start, end)
    //     TLBI VAAE1IS (all ASIDs, EL1, inner-shareable broadcast)
    //     DSB ISH (wait for all CPUs)
    
    // 3. Free vm_struct metadata (but NOT physical pages — no physical pages!)
    kfree(area);
}
```

---

## 5. Managed Device Resources (devres)

```
devm_ioremap: ioremap with automatic cleanup when device is removed
  Used by drivers to avoid manual iounmap in error paths

void __iomem *devm_ioremap(struct device *dev,
                            resource_size_t offset, resource_size_t size)
{
    // Allocate devres action:
    devres = devres_alloc(devm_ioremap_release, sizeof(*dr), GFP_KERNEL);
    
    // Normal ioremap:
    addr = ioremap(offset, size);
    
    // Store addr in devres structure:
    dr->addr = addr;
    
    // Register cleanup: called automatically on device unbind/remove:
    devres_add(dev, devres);  // when device removed: iounmap(dr->addr)
    
    return addr;
}

devm_ioremap_resource(dev, res):
    Combines: request_mem_region() + ioremap()
    Request prevents other drivers from claiming same MMIO range
    Auto-released on device removal
    Most modern drivers use this pattern

Typical driver usage:
    // In probe:
    base = devm_ioremap_resource(dev, &platform_device->resource[0]);
    if (IS_ERR(base)) return PTR_ERR(base);
    
    // Read 32-bit register:
    val = readl(base + REG_STATUS);     // ensures DSB before read on ARM64
    
    // Write 32-bit register:
    writel(0x1, base + REG_CONTROL);    // ensures DSB after write on ARM64
    
    // In remove: automatic iounmap via devres — no manual cleanup needed!

ARM64 accessor macros enforce correct ordering:
    readl(addr)  = DSB LD ST; *(u32*)addr  (read, then barrier)
    readl_relaxed(addr) = *(u32*)addr      (no barrier, driver must manage)
    writel(val, addr) = *(u32*)addr = val; DSB LD ST
    writel_relaxed(val, addr) = *(u32*)addr = val  (no barrier)
    
    Use _relaxed versions only when you manage barriers yourself (interrupt handlers, etc.)
```

---

## 6. Interview Questions & Answers

**Q1: What is the difference between ioremap() and ioremap_wc()? When would you use ioremap_wc()?**

`ioremap()` maps MMIO with Device nGnRE attributes. Device memory semantics:
- **Non-Gathering (nG)**: each write is a separate transaction, no merging
- **Non-Reordering (nR)**: writes in program order = writes at device in same order
- **Early Ack (E)**: CPU doesn't stall waiting for device to complete write

This gives strong ordering guarantees suitable for device control registers where order matters (e.g., must write command register AFTER writing address register).

`ioremap_wc()` maps with Write-Combining (WC) or Normal Non-Cacheable attributes. The CPU may:
- **Merge** multiple writes to adjacent addresses into a single bus transaction
- Reorder writes within the WC buffer (within barriers)
- Buffer writes and flush at cache line boundaries

This gives much better throughput for bulk writes. When to use:
- **Frame buffers**: CPU writes pixel data in a sequential stream; individual write ordering doesn't matter; merging 64 bytes into one bus transaction vs 16 × 4 bytes = 4× better throughput
- **GPU command rings**: large sequential write streams
- **PCIe burst writes**: devices that benefit from merged writes

Never use `ioremap_wc()` for control registers where write ordering is critical.

**Q2: What happens if you use `__va()` to access MMIO physical addresses?**

Undefined behavior / crash. `__va(pa) = pa + PAGE_OFFSET` maps into the Linux linear map, which only covers **RAM** regions pre-mapped at boot by `map_memory()`. MMIO physical addresses are NOT in the linear map. Accessing them via `__va()`:

1. If no page table entry exists: **synchronous abort** on ARM64 (Translation fault, level 0)
2. If by coincidence the PTE exists with Normal memory attributes but points to device registers: **incorrect behavior** — the CPU may cache reads (getting stale values), merge writes (missing register writes), or reorder operations

The kernel protects against accidental linear-map access to MMIO: `arch/arm64/mm/mmu.c` creates the linear map only for RAM regions from `memblock`. MMIO regions are never added to memblock (they are reserved/excluded).

---

## 7. Quick Reference

| ioremap Variant | ARM64 MAIR | Use Case |
|---|---|---|
| ioremap() | Device nGnRE | General MMIO (default) |
| ioremap_nocache() | Device nGnRE | Same as ioremap() |
| ioremap_wc() | Normal NC / Device GRE | Frame buffers, bulk writes |
| ioremap_uc() | Device nGnRnE | Legacy devices needing strictest ordering |
| devm_ioremap() | Device nGnRE | Managed resource (auto-cleanup) |

| ARM64 Device Register Access | Macro | Barrier |
|---|---|---|
| Read 32-bit | readl() | DSB LD before + after |
| Write 32-bit | writel() | DSB ST after |
| Read 64-bit | readq() | DSB LD |
| Relaxed read | readl_relaxed() | None |
| Relaxed write | writel_relaxed() | None |
