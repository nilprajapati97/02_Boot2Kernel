# 52-bit VA/PA: ARMv8.2 LPA — When and How It Is Enabled

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm (high-end server/HPC)

---

## 1. Concept Foundation

ARMv8.2 introduced two extensions that push beyond the standard 48-bit VA/PA limits:

- **LPA (Large Physical Address)**: Extends PA to 52 bits (up to 4 PB of physical memory)
- **LVA (Large Virtual Address)**: Extends VA to 52 bits (up to 4 PB of virtual address space)

These are both enabled via the `FEAT_LPA` feature in `ID_AA64MMFR0_EL1`. 52-bit support is critical for:
- Extreme-scale servers (cloud, HPC, AI training) with > 256 TB RAM
- GPU-compute systems (NVIDIA Grace, AMD CDNA with unified memory > 48-bit PA)
- Future-proofing for multi-petabyte memory pools in CXL/HBM systems

**Important restriction**: 52-bit VA/PA requires **64KB granule** (`TCR_EL1.TG0=0b01`). It cannot be used with 4KB or 16KB granules.

---

## 2. Why 64KB Granule Only?

The 52-bit extension works by repurposing bits in the page descriptor that are free in 64KB granule mode:

For a 64KB page:
- Page offset: 16 bits (bits[15:0])
- PA bits[47:16]: 32 bits in descriptor
- With LPA: bits[51:48] of the PA are stored in **previously reserved bits** of the descriptor (bits[15:12] or other implementation-specific positions)

For 4KB page:
- Page offset: 12 bits
- There are no spare descriptor bits available for PA[51:48] without breaking the standard descriptor format
- Hence LPA requires 64KB granule

---

## 3. Hardware Detection and Identification

```c
// arch/arm64/include/asm/cpufeature.h
// Feature: ARM64_HAS_LVA (Large Virtual Address)
// Feature: ARM64_HAS_LPA2 (ARMv8.7 LPA2 — extends 4KB/16KB to 52-bit PA)

// Detecting LPA:
// ID_AA64MMFR0_EL1.PARange field:
//   0b0110 = 52-bit PA supported (LPA)
//   0b0101 = 48-bit PA (standard)

static bool system_supports_lpa(void)
{
    u64 mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
    return cpuid_feature_extract_unsigned_field(mmfr0,
        ID_AA64MMFR0_EL1_PARANGE_SHIFT) >= ID_AA64MMFR0_EL1_PARANGE_52;
}

// TCR_EL1.IPS encoding for 52-bit PA:
// IPS[34:32] = 0b110 (was 0b101 for 48-bit)
#define TCR_IPS_52BIT   (6UL << TCR_IPS_SHIFT)
```

---

## 4. 52-bit VA Layout (64KB Granule)

With 64KB granule and 52-bit VA:

```
Page size:    64 KB (2^16 bytes)
Page offset:  bits[15:0]  (16 bits)

Page table levels (3 levels for 52-bit VA with 64KB granule):
  Level 0 (L0): bits[51:42]  — 10-bit index → 1024 entries (new level for >48-bit)
  Level 1 (L1): bits[41:29]  — 13-bit index → 8192 entries
  Level 2 (L2): bits[28:16]  — 13-bit index → 8192 entries (leaf → 64KB page)

Wait — this needs careful recount for 52-bit/64KB:
  With T0SZ=12: VA = 52 bits
  Level 0: bits[51:42] = 10 bits → 8 entries (ARM64 uses partial L0)
  Level 1: bits[41:29] = 13 bits → 8192 entries × 8 bytes = 64 KB/table
  Level 2: bits[28:16] = 13 bits → leaf (64 KB page)

Actually for 64KB granule with 52-bit VA:
  L0 concatenated tables (aligned):
    L0: va[51:42] — 10 bits → 1024 entries (entries point to L1)
    L1: va[41:29] — 13 bits → 8192 entries (each 512 GB block or L2 ptr)
    L2: va[28:16] — 13 bits → 8192 entries (each 64 KB page or 512 MB block)
```

---

## 5. 52-bit PA in Page Descriptors

For 64KB granule, the page descriptor stores PA[51:48] in bits [15:12] of the descriptor:

```
Standard 64KB page descriptor (48-bit PA):
Bits[63:12] contain PA[47:12] (lower 12 bits are offset, zero in descriptor)

Extended 64KB page descriptor (52-bit PA / LPA):
Bits[15:12] = PA[51:48]  (the extra 4 PA bits)
Bits[47:16] = PA[47:16]  (standard PA bits)
```

```
Example: PA = 0x000F_1234_5678_0000 (52-bit PA, non-zero bits[51:48] = 0xF)

Standard descriptor format:
  Normal PA bits [47:16] → 0x1234_5678
  Extra PA bits [51:48] → 0xF stored in descriptor bits [15:12]
  
  Full descriptor: 
  | bits[63:48] | bits[47:16] | bits[15:12] | bits[11:0] |
  | attributes  | PA[47:16]   | PA[51:48]   | flags      |
```

---

## 6. Linux Kernel Support for LPA

```c
// arch/arm64/include/asm/pgtable-prot.h
// LPA page table entry helpers (64KB granule):
#ifdef CONFIG_ARM64_64K_PAGES
// The page size is 64KB, so PA extension bits fit in bits[15:12]
#define phys_to_pte(phys) \
    (((phys) >> 16) << 16) | (((phys) >> 48) << 12)
    // Pack PA[47:16] into bits[47:16] and PA[51:48] into bits[15:12]
#endif

// arch/arm64/mm/mmu.c
static void __init map_range(...)
{
    // When LPA is active, PA > 2^48 can be mapped
    // The mapping code handles the descriptor bit packing automatically
}
```

### TCR_EL1 for LPA

```c
// With LPA enabled (64KB granule, 52-bit VA):
TCR_EL1 value:
  TG0 = 0b01   (64KB granule for TTBR0)
  TG1 = 0b11   (64KB granule for TTBR1)
  T0SZ = 12    (64 - 12 = 52-bit VA)
  T1SZ = 12    (64 - 12 = 52-bit VA)
  IPS  = 0b110 (52-bit PA)
```

---

## 7. ARMv8.7 LPA2 — 52-bit PA with 4KB and 16KB Granule

ARMv8.7 introduces **LPA2** (`FEAT_LPA2`) which extends 52-bit PA support to 4KB and 16KB granules (fixing the 64KB-only restriction):

```
LPA2 (ARMv8.7):
  Works with: 4KB, 16KB, AND 64KB granules
  Uses a different encoding: descriptor bit extension via "OA[51:50]" stored
  in descriptor bits [9:8] (previously output address extension)
  
  Detection: ID_AA64MMFR0_EL1.TGran4_2 field (not just PARange)

Linux 6.x adds CONFIG_ARM64_LPA2:
  Enables 52-bit PA support for 4KB and 16KB pages
  Changes page table entry format slightly
```

---

## 8. Real-World Use Cases for LPA

### NVIDIA Grace Hopper Superchip

```
Grace CPU (ARM Neoverse V2): 512 GB LPDDR5X per module
Hopper GPU (HBM2e): 96 GB HBM per module
NVLink-C2C interconnect: unified 576 GB PA space
Total: exceeds 256 TB? No, but NVLink address space requires careful PA management.

Future GH200 clusters: hundreds of nodes × 576 GB = well into TB range
LPA ensures PA addressing is sufficient.
```

### AMD CDNA with Unified Memory

```
AMD CDNA2/3 (Instinct MI250/MI300):
  MI300A: CPU + GPU on same die, 128–192 GB HBM
  Future: multiple GPU tiles × large HBM → possible > 256 GB per device
  With many devices: PCIe/CXL address space expansion needs LPA
```

---

## 9. Linux Kernel Build Configuration

```
CONFIG_ARM64_64K_PAGES=y       # Required for LPA (ARMv8.2)
CONFIG_ARM64_VA_BITS=52        # Enable 52-bit VA
CONFIG_ARM64_PA_BITS=52        # Enable 52-bit PA (implied by LPA detection)

OR (ARMv8.7+):
CONFIG_ARM64_4K_PAGES=y        # Standard 4KB pages
CONFIG_ARM64_VA_BITS=52        # 52-bit VA
CONFIG_ARM64_LPA2=y            # LPA2 for 4KB/16KB granule
```

---

## 10. Interview Questions & Answers

**Q1: What is LPA and why is it restricted to 64KB granule?**

LPA (Large Physical Address) is the ARMv8.2 feature that extends the physical address space to 52 bits (4 petabytes). It requires 64KB granule because the extra 4 PA bits [51:48] are stored in descriptor bits [15:12] — positions that happen to be unused/available in the 64KB page descriptor format. For 4KB granule, those bits are used for other purposes, so 52-bit PA cannot be stored without redesigning the descriptor format. ARMv8.7's LPA2 solves this by using a different encoding that works for all granule sizes.

**Q2: Why would a system need more than 256 TB of physical memory?**

256 TB (48-bit PA) represents the current maximum for standard ARM64. Systems that approach or exceed this include: large-scale HPC clusters using CXL memory expansion (aggregated DRAM+CXL can exceed 256 TB per node in future designs), AI training systems with massive HBM+DRAM pools, in-memory databases on extreme-scale servers, and unified memory architectures where GPU memory is in the same PA space as CPU RAM. LPA provides headroom for these future systems.

**Q3: How does Linux detect and enable LPA at runtime?**

Linux reads `ID_AA64MMFR0_EL1.PARange` during boot capability detection. If it reports 52-bit PA support (value 0b0110), the `ARM64_HAS_LPA` capability is set. The kernel then sets `TCR_EL1.IPS=0b110` (52-bit PA) during `__cpu_setup()`. The build must also have `CONFIG_ARM64_64K_PAGES` and `CONFIG_ARM64_VA_BITS=52` enabled. At runtime, physical memory above 2^48 bytes is discovered via ACPI/UEFI memory maps and added to the memory allocator's pool.

---

## 11. Quick Reference

| Feature | ARMv8.2 LPA | ARMv8.7 LPA2 |
|---|---|---|
| Max PA | 52 bits (4 PB) | 52 bits (4 PB) |
| Max VA | 52 bits (4 PB) | 52 bits (4 PB) |
| Granule | 64KB only | 4KB, 16KB, 64KB |
| Detection | ID_AA64MMFR0.PARange=0b0110 | ID_AA64MMFR0.TGran4_2 |
| TCR.IPS | 0b110 | 0b110 |
| TCR.T0SZ | 12 (52-bit VA) | 12 |
| Linux Config | ARM64_64K_PAGES + VA_BITS=52 | ARM64_LPA2 |
| PA descriptor bits | [15:12] store PA[51:48] | Different encoding |
