# early_mm_init() — ARM32 vs ARM64 Design Details

## 1. Does ARM64 Have early_mm_init()?

**No.** `early_mm_init()` is an **ARM32-only function** wrapped in `#ifdef CONFIG_MMU`.

```c
/* arch/arm/kernel/setup.c */
#ifdef CONFIG_MMU
    early_mm_init(mdesc);
#endif
```

ARM64's `setup_arch()` has no equivalent call. The tasks performed by ARM32's `early_mm_init()` are either not needed on ARM64 or are handled differently:

| ARM32 early_mm_init task | ARM64 equivalent |
|--------------------------|-----------------|
| `build_mem_type_table()` | Not needed — ARMv8 attrs are standardized |
| `early_paging_init()` (PV fixup) | Not needed — 64-bit VA eliminates the constraint |

---

## 2. Why ARM32 Needs a Memory Type Table but ARM64 Does Not

### ARM32: CPU-Dependent Cache Policy Encoding

ARM32 page table entries encode cache/memory type in `TEX[2:0] + C + B` bits. The exact meaning of these bits **changed across ARM architecture versions**:

| Architecture | Cache Encoding | Example |
|-------------|---------------|---------|
| ARMv4/v5 | `C=1, B=1` = Writeback (no TEX) | SA1110 |
| ARMv6 | `TEX[2:0] + C + B` = full policy | ARM1136 |
| ARMv7 (TEX remap off) | `TEX[2:0] + C + B` | Cortex-A8 |
| ARMv7 (TEX remap on) | `TEX[2:0]` indexes PRRR/NMRR registers | Cortex-A9 |
| LPAE | `AttrIndx[2:0]` indexes MAIR0/MAIR1 registers | Cortex-A15 |

Because a single kernel binary must run on multiple ARM32 generations, `build_mem_type_table()` detects the CPU type at runtime and fills `mem_types[]` with the correct encoding for the current CPU.

### ARM64: Standardized Memory Attribute Register

ARMv8 (ARM64) uses **`MAIR_EL1`** (Memory Attribute Indirection Register, EL1) — a hardware register with 8 attribute slots, each 8 bits. The `AttrIndx[2:0]` field in a page table descriptor indexes into this register.

The kernel sets `MAIR_EL1` once during `cpu_init()` with well-defined values:

```c
/* arch/arm64/include/asm/pgtable-hwdef.h */
#define MT_DEVICE_nGnRnE    0  /* Device, non-gathering, non-reordering, no early-write ack */
#define MT_DEVICE_nGnRE     1  /* Device, non-gathering, non-reordering */
#define MT_DEVICE_GRE       2  /* Device, gathering, reordering, early-write ack */
#define MT_NORMAL_NC        3  /* Normal, non-cacheable */
#define MT_NORMAL           4  /* Normal, cacheable (inner/outer WB, RA, WA) */
#define MT_NORMAL_WT        5  /* Normal, Write-Through */
```

No runtime table building needed — the hardware register defines the attributes, and they are the same for all ARMv8 implementations.

---

## 3. ARM32 mem_types[] vs ARM64 MAIR_EL1

### ARM32: Software Table (mem_types[])

```
  mem_types[MT_MEMORY_RW]
    .prot_pte  = L_PTE_MT_WRITEALLOC | L_PTE_DIRTY | L_PTE_YOUNG
    .prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_WB
    .domain    = DOMAIN_KERNEL
```

`create_mapping()` reads these fields and ORs them into page table descriptors:

```c
pgd |= mem_types[map.type].prot_sect;
```

### ARM64: Hardware Register (MAIR_EL1)

```
MAIR_EL1[bits 39:32] = 0xFF (MT_NORMAL = inner/outer WB, RA, WA)

Page table PTE:
  AttrIndx[2:0] = MT_NORMAL (4)  → hardware looks up MAIR_EL1[39:32]
```

No software array. The hardware does the lookup. This is simpler and **faster** (no software indirection).

---

## 4. ARM32 LPAE PV Fixup vs ARM64 Physical Address

### ARM32 with LPAE: The 32-bit VA Problem

ARM32's virtual address space is 32 bits. `PAGE_OFFSET` (typically 0xC0000000) splits it: user space below, kernel above. The physical-to-virtual mapping formula is:

$$\text{phys} = \text{virt} - \text{PAGE\_OFFSET} + \text{PHYS\_OFFSET}$$

This assumes `PHYS_OFFSET` is a compile-time constant. Works for DRAM ≤ 4GB. Fails for Keystone2 with DRAM at 32GB (0x800000000).

`early_paging_init()` patches the kernel binary at boot to change all embedded `PHYS_OFFSET` constants.

```
Compile time:   PHYS_OFFSET = 0x80000000 (assumed)
Boot time:      mdesc->pv_fixup() returns 0x780000000 (the actual offset)
After fixup:    all __pa()/__va() calls use 0x800000000
```

### ARM64: 64-bit VA Eliminates the Constraint

ARM64's virtual address space is 48 bits (or 52 bits with LPA). There is **no PAGE_OFFSET constraint** relative to physical memory. PHYS_OFFSET can be any 48-bit value — there is no 4GB ceiling.

```c
/* arch/arm64/include/asm/memory.h */
#define __phys_to_virt(x)   ((unsigned long)((x) - PHYS_OFFSET) | PAGE_OFFSET)
#define __virt_to_phys(x)   ((phys_addr_t)((x) & ~PAGE_OFFSET) + PHYS_OFFSET)
```

`PHYS_OFFSET` on ARM64 is **discovered at runtime** from the FDT memory node and stored as a variable — no patching needed. This is a fundamental architectural advantage of 64-bit.

---

## 5. Page Table Level Comparison

| | ARM32 (no LPAE) | ARM32 (LPAE) | ARM64 (4KB pages) |
|--|-----------------|--------------|-------------------|
| Levels | 2 (PGD, PTE) | 3 (PGD, PMD, PTE) | 4 (PGD, PUD, PMD, PTE) |
| PGD size | 4KB (4096 × 4B) | 5 pages | 4KB |
| Page size | 4KB or 64KB sections | 4KB | 4KB, 16KB, 64KB |
| Physical addr bits | 32 | 40 | 48 (up to 52 with LPA) |
| Descriptor size | 32-bit | 64-bit | 64-bit |
| Hardware walker | CP15 TTBR0/TTBR1 | CP15 TTBR0/TTBR1 (64-bit) | TTBR0_EL1/TTBR1_EL1 |

`build_mem_type_table()` must handle both 32-bit and 64-bit descriptor formats on ARM32. ARM64 always uses 64-bit descriptors — no runtime switching.

---

## 6. Domain Support

### ARM32: Hardware Domains (ARMv7 and Earlier)

ARM32 has 16 hardware domains. `build_mem_type_table()` assigns each memory type a domain:

```c
mem_types[MT_MEMORY_RW].domain   = DOMAIN_KERNEL;
mem_types[MT_DEVICE].domain      = DOMAIN_IO;
mem_types[MT_LOW_VECTORS].domain = DOMAIN_USER;
```

The Domain Access Control Register (DACR) can quickly enable/disable domains without TLB flush — useful for fast context switches.

### ARM64: No Hardware Domains

ARM64 removed domain support. Permission control uses:
- `AP[2:1]` bits in the page descriptor (read/write, user/kernel)
- `UXN/PXN` (User/Privileged Execute Never)
- Stage-2 page tables for virtualization

This is more granular than domains and avoids the DACR being a single-point-of-control that could be exploited.

---

## 7. Cache Policy Comparison

### ARM32 Cache Policies (set by build_mem_type_table)

| Policy | TEX | C | B | Description |
|--------|-----|---|---|-------------|
| Write-through | 000 | 1 | 0 | Read cached, writes go to RAM |
| Write-back no alloc | 000 | 1 | 1 | Read/write cached, no write-alloc |
| Write-back write-alloc | 001 | 1 | 1 | Full write-back with write-allocate |
| Non-cacheable | 000 | 0 | 0 | No caching |
| Strongly-ordered | 000 | 0 | 0 | Device/uncached, strict ordering |

### ARM64 Cache Policies (MAIR_EL1)

| MAIR encoding | Description |
|---------------|-------------|
| 0xFF | Normal, inner/outer WB, RA, WA |
| 0x44 | Normal, non-cacheable |
| 0x00 | Device, nGnRnE (strongly ordered) |
| 0x04 | Device, nGnRE (peripheral) |
| 0xBB | Normal, write-through |

ARM64's MAIR approach is cleaner: 8 configurable slots, referenced by 3-bit index. ARM32's TEX remap adds another layer of indirection that ARM64 eliminates.

---

## 8. Summary Comparison Table

| Feature | ARM32 early_mm_init | ARM64 equivalent |
|---------|--------------------|--------------------|
| Function exists | Yes (`early_mm_init`) | No |
| Memory type table | `mem_types[]` built at runtime | MAIR_EL1 register (set once) |
| Cache policy encoding | TEX+C+B, CPU-specific | MAIR_EL1 AttrIndx — standardized |
| Domain support | Yes (DACR + mem_types.domain) | No |
| PV fixup for >4GB phys | Yes (Keystone2 LPAE) | Not needed (64-bit VA) |
| PHYS_OFFSET | Compile-time constant (patchable) | Runtime variable |
| Code complexity | Medium-high (CPU-dependent branches) | Low |
| `#ifdef CONFIG_MMU` guard | Yes | Not applicable |
| Called from setup_arch | Yes | No |
