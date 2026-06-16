# MAIR_EL1 Programming and Linux Kernel Configuration

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm

---

## 1. Boot-Time MAIR_EL1 Initialization

```
MAIR_EL1 must be configured BEFORE enabling the MMU.
Sequence in arch/arm64/kernel/head.S:

  1. Compute TCR_EL1 value (T0SZ, TG, IRGN, ORGN, SH, IPS...)
  2. Set MAIR_EL1
  3. Set TTBR0_EL1 (user page table base) and TTBR1_EL1 (kernel page table)
  4. ISB (ensure all previous MSR instructions complete)
  5. Set SCTLR_EL1.M = 1 (enable MMU)
  6. ISB (flush pipeline after MMU enable)

If MAIR_EL1 is set AFTER MMU enable:
  Existing PTEs already reference slots by index.
  Changing MAIR slot values after the fact is allowed BUT:
  All TLB entries that cached the old attribute interpretation
  are now stale. Must flush all TLBs (TLBI VMALLE1IS) after MAIR change.
  
  Linux NEVER changes MAIR_EL1 after boot. It's set once and constant.
```

---

## 2. Linux MAIR_EL1 Slot Assignments

```c
// arch/arm64/include/asm/pgtable-hwdef.h

// Slot indices (used in PTE AttrIndx[2:0]):
#define MT_DEVICE_nGnRnE    0  // 0b000 → MAIR byte 0
#define MT_DEVICE_nGnRE     1  // 0b001 → MAIR byte 1
#define MT_DEVICE_GRE       2  // 0b010 → MAIR byte 2
#define MT_NORMAL_NC        3  // 0b011 → MAIR byte 3
#define MT_NORMAL           4  // 0b100 → MAIR byte 4
#define MT_NORMAL_WT        5  // 0b101 → MAIR byte 5
// Slots 6, 7 unused in standard Linux kernel

// MAIR byte values for each slot:
#define MAIR_ATTR_DEVICE_nGnRnE   0x00  // nGnRnE device
#define MAIR_ATTR_DEVICE_nGnRE    0x04  // nGnRE device
#define MAIR_ATTR_DEVICE_GRE      0x0c  // GRE device
#define MAIR_ATTR_NORMAL_NC       0x44  // Normal, Outer NC + Inner NC
#define MAIR_ATTR_NORMAL          0xff  // Normal, Outer WB RA WA + Inner WB RA WA
#define MAIR_ATTR_NORMAL_WT       0xbb  // Normal, WB RA (outer) + WB RA (inner)
// Note: 0xBB is outer=0b1011 inner=0b1011 (WB, RA, no WA)

// Assembling the full 64-bit MAIR_EL1 value:
#define MAIR_EL1_SET \
  MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRnE, MT_DEVICE_nGnRnE)  |  /* bits[7:0]   */ \
  MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRE,  MT_DEVICE_nGnRE)   |  /* bits[15:8]  */ \
  MAIR_ATTRIDX(MAIR_ATTR_DEVICE_GRE,    MT_DEVICE_GRE)     |  /* bits[23:16] */ \
  MAIR_ATTRIDX(MAIR_ATTR_NORMAL_NC,     MT_NORMAL_NC)      |  /* bits[31:24] */ \
  MAIR_ATTRIDX(MAIR_ATTR_NORMAL,        MT_NORMAL)         |  /* bits[39:32] */ \
  MAIR_ATTRIDX(MAIR_ATTR_NORMAL_WT,     MT_NORMAL_WT)         /* bits[47:40] */

// MAIR_EL1 = 0x00_BB_FF_44_0C_04_00_00 (hex)
//   byte 7 (slot 7): 0x00 (unused)
//   byte 6 (slot 6): 0x00 (unused)
//   byte 5 (slot 5): 0xBB (Normal WT)
//   byte 4 (slot 4): 0xFF (Normal WB RA WA)
//   byte 3 (slot 3): 0x44 (Normal NC)
//   byte 2 (slot 2): 0x0C (Device GRE)
//   byte 1 (slot 1): 0x04 (Device nGnRE)
//   byte 0 (slot 0): 0x00 (Device nGnRnE)
// 
// Actual hex (reading from high to low byte):
// = 0x000000BBFF440C04  (depending on byte order)
// Stored as: bits[7:0]=0x00, bits[15:8]=0x04, bits[23:16]=0x0C,
//            bits[31:24]=0x44, bits[39:32]=0xFF, bits[47:40]=0xBB

// Assembly (head.S snippet):
// ldr  x5, =MAIR_EL1_SET
// msr  MAIR_EL1, x5
// isb
```

---

## 3. PTE AttrIndx Field and Linux PTE Construction

```c
// arch/arm64/include/asm/pgtable-prot.h

// PTE_ATTRINDX macro places index into PTE bits[4:2]:
#define PTE_ATTRINDX(t)      ((t) << 2)
#define PTE_ATTRINDX_MASK    (7 << 2)     // mask for bits[4:2]

// Example page protection flags:
#define PAGE_KERNEL         __pgprot(PTE_VALID | PTE_TYPE_PAGE | PTE_AF | \
                                    PTE_SHARED | PTE_PXN | PTE_UXN | \
                                    PTE_DIRTY | PTE_WRITE | \
                                    PTE_ATTRINDX(MT_NORMAL))

#define PAGE_KERNEL_RO      __pgprot(PTE_VALID | PTE_TYPE_PAGE | PTE_AF | \
                                    PTE_SHARED | PTE_PXN | PTE_UXN | \
                                    PTE_RDONLY | \
                                    PTE_ATTRINDX(MT_NORMAL))

#define PAGE_KERNEL_EXEC    __pgprot(PTE_VALID | PTE_TYPE_PAGE | PTE_AF | \
                                    PTE_SHARED | PTE_UXN | \
                                    PTE_DIRTY | PTE_WRITE | \
                                    PTE_ATTRINDX(MT_NORMAL))

// Device memory page:
#define pgprot_noncached(prot)   __pgprot_modify(prot, PTE_ATTRINDX_MASK, \
                                     PTE_ATTRINDX(MT_DEVICE_nGnRnE))

#define pgprot_writecombine(prot) __pgprot_modify(prot, PTE_ATTRINDX_MASK, \
                                     PTE_ATTRINDX(MT_DEVICE_GRE))

// Non-cacheable Normal (for DMA coherent buffers):
#define pgprot_dmacoherent(prot) __pgprot_modify(prot, PTE_ATTRINDX_MASK, \
                                     PTE_ATTRINDX(MT_NORMAL_NC))
```

---

## 4. ioremap Functions and Their MAIR Usage

```c
// Kernel MMIO mapping → nGnRnE (slot 0):
void __iomem *ioremap(phys_addr_t phys_addr, size_t size)
{
    return ioremap_prot(phys_addr, size, pgprot_device(PAGE_KERNEL));
    // pgprot_device: MT_DEVICE_nGnRnE → strict ordering
}

// Write-combining mapping → GRE (slot 2):
void __iomem *ioremap_wc(phys_addr_t phys_addr, size_t size)
{
    return ioremap_prot(phys_addr, size, pgprot_writecombine(PAGE_KERNEL));
    // pgprot_writecombine: MT_DEVICE_GRE → gathering allowed
}

// Normal cached mapping (for RAM):
void *memremap(resource_size_t offset, size_t size, unsigned long flags)
    // MEMREMAP_WB: MT_NORMAL (0xFF)
    // MEMREMAP_WT: MT_NORMAL_WT (0xBB)
    // MEMREMAP_WC: MT_DEVICE_GRE (0x0C) or platform-specific

// DMA coherent allocation:
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
    // On non-coherent SoC: MT_NORMAL_NC (0x44)
    // On coherent SoC: MT_NORMAL (0xFF)
    // Determined by dev_is_dma_coherent(dev) and arch's dma_ops
```

---

## 5. Verifying MAIR Configuration

```
From userspace / debug:
  No direct userspace access to MAIR_EL1 (privileged register).
  
  Use hwcap or sysfs to infer (indirect):
  /proc/cpuinfo shows CPU model → infer MAIR support
  
From kernel:
  // Read back MAIR_EL1 to verify:
  u64 mair = read_sysreg(mair_el1);
  pr_info("MAIR_EL1 = 0x%016llx\n", mair);
  
  // Extract slot N:
  u8 attr_n = (mair >> (slot * 8)) & 0xFF;
  pr_info("MAIR slot %d = 0x%02x\n", slot, attr_n);
  
  // Verify MAIR from PTE AttrIndx:
  pte_t pte = *ptep;
  int attrindx = (pte_val(pte) >> 2) & 0x7;
  u8 attr = (mair >> (attrindx * 8)) & 0xFF;
  pr_info("Page at %lx uses MAIR[%d]=0x%02x\n", addr, attrindx, attr);

dmesg example:
  MAIR_EL1 = 0x000000BBFF440C0400
  Slot 0: 0x00 → Device nGnRnE ✓
  Slot 4: 0xFF → Normal WB RA WA ✓
```

---

## 6. MAIR in Hypervisor / Stage 2

```
In KVM/ARM64 with Stage 2 translation:

Stage 1 (guest OS):
  Guest sets its own MAIR_EL1 → defines guest's memory type slots
  Guest PTEs use AttrIndx into guest's MAIR_EL1
  Stage 1 output: guest PA (IPA) with memory attributes

Stage 2 (hypervisor, KVM):
  VTTBR_EL2 / VTCR_EL2 — Stage 2 page table
  Stage 2 descriptors have MemAttr[3:0] field (different encoding)
  Stage 2 MemAttr encoding:
    Bits[3:2] = outer memory type
    Bits[1:0] = inner memory type
    (Encoding similar to MAIR nibbles)
  
  MAIR_EL1 does NOT apply to Stage 2.
  Stage 2 memory attributes are in the Stage 2 descriptor directly.

Combined attributes:
  When both Stage 1 and Stage 2 are active:
  Final memory attribute = minimum(Stage1 attr, Stage2 attr)
  Hardware picks the MORE RESTRICTIVE attribute of the two stages.
  If Stage 1 says WB (0xFF) but Stage 2 says NC (0x44) → NC wins.
  This prevents a guest from making memory more permissive than the hypervisor allows.
```

---

## 7. Interview Questions & Answers

**Q1: Describe how the Linux kernel sets up MAIR_EL1 and at what point in the boot sequence.**

Linux initializes `MAIR_EL1` in `arch/arm64/kernel/head.S` **before** enabling the MMU. The 64-bit register is loaded with a precomputed constant that packs 6 attribute bytes into slots 0–5: slot 0 = `0x00` (Device nGnRnE), slot 1 = `0x04` (Device nGnRE), slot 2 = `0x0C` (Device GRE), slot 3 = `0x44` (Normal NC), slot 4 = `0xFF` (Normal WB RA WA), slot 5 = `0xBB` (Normal WT). An `ISB` ensures the write completes before the MMU enable (`SCTLR_EL1.M=1`). Once set, MAIR_EL1 is never changed during Linux operation. All PTE creation functions use `PTE_ATTRINDX(MT_xxx)` macros that embed the slot index in PTE bits[4:2].

**Q2: What happens if you change MAIR_EL1 while the MMU is enabled and there are active page mappings?**

Changing MAIR slot values while the MMU is running and PTEs exist that reference those slots creates a **stale TLB problem**. TLB entries cache the memory attribute along with the translation — they captured the old MAIR interpretation at TLB fill time. After changing MAIR, the TLB still holds the old attribute (e.g., old slot 4 = NC, new slot 4 = WB). CPUs accessing mapped pages will use the OLD attribute from TLB, not the new MAIR value, until the TLB is invalidated. The fix: `TLBI VMALLE1IS` (or appropriate TLBI) after changing MAIR, followed by `DSB ISH` and `ISB`. Linux avoids this problem entirely by never changing MAIR after boot.

---

## 8. Quick Reference

| Slot | Index | Byte | Name | ioremap function |
|---|---|---|---|---|
| 0 | 0b000 | 0x00 | MT_DEVICE_nGnRnE | `ioremap()` |
| 1 | 0b001 | 0x04 | MT_DEVICE_nGnRE | (direct pgprot) |
| 2 | 0b010 | 0x0C | MT_DEVICE_GRE | `ioremap_wc()` |
| 3 | 0b011 | 0x44 | MT_NORMAL_NC | DMA coherent (non-coherent SoC) |
| 4 | 0b100 | 0xFF | MT_NORMAL | All normal memory |
| 5 | 0b101 | 0xBB | MT_NORMAL_WT | `memremap(MEMREMAP_WT)` |
