# 64-bit VA: Why Only 48 or 52 Bits Are Used

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

A 64-bit CPU has 64-bit pointers, but ARM64 does not use all 64 bits for virtual addresses. This is a deliberate design choice combining physical feasibility with future extensibility.

**Why not 64-bit VA?**

1. **Hardware cost**: Every extra VA bit doubles the TLB entry count for full coverage. A full 64-bit TLB would be physically enormous.
2. **Physical memory limits**: No system today has 2^48 bytes (256 TB) of RAM. Extending to full 64-bit would be wasteful.
3. **Reserved bits for OS metadata**: The upper bits not used for VA translation can carry OS-specific metadata (Pointer Authentication codes, Memory Tagging Extension tags).
4. **Transistor budget**: Page table walkers, TLBs, and caches need only cover the implemented VA range.

**The solution**: ARM defines a variable VA width controlled by software via `TCR_EL1.T0SZ` and `TCR_EL1.T1SZ`. The hardware only checks the top bits for canonical form; unused upper bits carry no address meaning.

---

## 2. VA Width Options on ARM64

### ARMv8.0 — Baseline: 48-bit VA Maximum

```
VA space: 2^48 = 256 TB
  User space (TTBR0):   0x0000_0000_0000_0000 → 0x0000_FFFF_FFFF_FFFF
  Kernel space (TTBR1): 0xFFFF_0000_0000_0000 → 0xFFFF_FFFF_FFFF_FFFF

Address hole (invalid VA):
  0x0001_0000_0000_0000 → 0xFFFE_FFFF_FFFF_FFFF
  (bits [62:48] must all match bit[47] — canonical form)
```

### ARMv8.2 (LPA) — 52-bit VA (FEAT_LVA)

```
VA space: 2^52 = 4 PB (peta bytes)
  Requires: 64KB granule only
  TCR_EL1.T0SZ = 12 (for 52-bit: 64 - 12 = 52)
  
  User space (TTBR0):   0x0000_0000_0000_0000 → 0x000F_FFFF_FFFF_FFFF
  Kernel space (TTBR1): 0xFFF0_0000_0000_0000 → 0xFFFF_FFFF_FFFF_FFFF
```

---

## 3. TCR_EL1 Controls the Effective VA Width

`T0SZ` and `T1SZ` fields in `TCR_EL1` define the address space size:

```
VA_bits = 64 - TxSZ

T0SZ = 16: 64 - 16 = 48-bit user VA  (Linux default for 4KB granule)
T0SZ = 25: 64 - 25 = 39-bit user VA  (Linux default before 5.14 on some configs)
T0SZ = 12: 64 - 12 = 52-bit user VA  (LPA, 64KB granule only)
```

In Linux:
```c
// arch/arm64/include/asm/memory.h
#ifdef CONFIG_ARM64_VA_BITS_48
#define VA_BITS         48
#elif CONFIG_ARM64_VA_BITS_52
#define VA_BITS         52
#else
#define VA_BITS         39
#endif

#define VA_BITS_MIN     (CONFIG_ARM64_VA_BITS_MIN)
```

```c
// arch/arm64/kernel/head.S — set TCR_EL1 at boot
// T0SZ = 64 - VA_BITS for user space
// T1SZ = 64 - VA_BITS for kernel space
```

---

## 4. Canonical Address Rule

In AArch64, a valid VA must have bits [63:VA_BITS] all equal to bit [VA_BITS - 1]:

**For 48-bit VA**:
```
Bits [63:48] must match bit [47]:
  Valid user VA:   bits[63:48] = 0x0000  (bit 47 = 0)
  Valid kernel VA: bits[63:48] = 0xFFFF  (bit 47 = 1)
  
  Non-canonical (invalid): 0x0001_xxxx_xxxx_xxxx — ACCESS FAULT
```

**For 52-bit VA**:
```
Bits [63:52] must match bit [51]:
  Valid user:   bits[63:52] = 0x0000
  Valid kernel: bits[63:52] = 0xFFF0
```

The ARM MMU hardware checks this. A non-canonical address immediately faults with a Translation Fault at the first level.

---

## 5. Why Linux Uses 48-bit (Not 52-bit) as Default

1. **4KB granule compatibility**: 52-bit VA requires 64KB granule (LPA). Most ARM64 systems use 4KB granule for better memory overhead granularity.
2. **TLB overhead**: 52-bit VA requires L0 page table level (4 levels for 4KB granule already covers 48 bits perfectly).
3. **Sufficient address space**: 256 TB per process + 256 TB kernel is vastly more than any current system uses.
4. **Pointer packing**: The upper bits of 48-bit pointers (bits [63:48]) can store metadata (PAC codes, MTE tags).

---

## 6. ARM64 Linux Kernel Configuration

```
CONFIG_ARM64_VA_BITS choices:
  39 — For systems with small page tables (e.g., embedded)
  48 — Standard choice (most ARM64 Linux systems)
  52 — LPA systems (64KB granule only)

CONFIG_ARM64_4K_PAGES   — 4KB granule (most common)
CONFIG_ARM64_16K_PAGES  — 16KB granule (Apple Silicon: M1/M2)
CONFIG_ARM64_64K_PAGES  — 64KB granule (some server/HPC)
```

Apple Silicon (M1/M2) uses 16KB pages — this changes VA layout and page table sizing.

---

## 7. Practical Implication: Pointer Arithmetic in Linux

```c
// arch/arm64/include/asm/memory.h
// Sign-extension for kernel addresses:
#define __phys_to_virt(x)    ((unsigned long)((x) - PHYS_OFFSET) | PAGE_OFFSET)
#define __virt_to_phys(x)    ((phys_addr_t)((x) & ~PAGE_OFFSET) + PHYS_OFFSET)

// PAGE_OFFSET for 48-bit VA:
// CONFIG_ARM64_VA_BITS=48:
#define PAGE_OFFSET  UL(0xffff000000000000)
// = bit[47] sign-extended = 0xFFFF_0000_0000_0000

// All kernel virtual addresses have bit[63:48] = 0xFFFF
```

---

## 8. Interview Questions & Answers

**Q1: Why doesn't ARM64 use all 64 bits for virtual addresses?**

Using all 64 bits would be physically impractical and wasteful. TLBs, page table walkers, and address comparison logic all scale with the VA width. A 256 TB (48-bit) address space is already far beyond what any current server can populate with RAM. The unused upper bits (for 48-bit VA: bits [63:48]) serve as metadata carriers for features like Pointer Authentication (PAC), Memory Tagging Extension (MTE), and Top Byte Ignore (TBI). This dual-use design makes the architecture more efficient.

**Q2: What is the "canonical address hole" and what happens if you access a non-canonical address?**

The canonical address hole is the range of VAs where bits [63:VA_BITS] don't match bit [VA_BITS-1]. For 48-bit VA, this is 0x0001_0000_0000_0000 to 0xFFFE_FFFF_FFFF_FFFF. If code accesses a non-canonical address (e.g., due to a bug in pointer arithmetic), the ARM64 MMU immediately generates a Translation Fault at level 0 (before any page table walk). Linux handles this as a SIGSEGV to userspace or a kernel panic if it occurs in the kernel.

**Q3: How does Linux ARM64 handle 39-bit vs 48-bit VA systems?**

The kernel configuration `CONFIG_ARM64_VA_BITS` selects the VA size at build time. This sets the `VA_BITS` constant which is used to compute `T0SZ`/`T1SZ` values written to `TCR_EL1` at boot. `PAGE_OFFSET` and all kernel VA layout constants are adjusted accordingly. A kernel built for 48-bit VA cannot run correctly on a system that only supports 39-bit VA (the TCR write would be out of range for the hardware).

---

## 9. Quick Reference

| Feature | 39-bit VA | 48-bit VA | 52-bit VA |
|---|---|---|---|
| User space top | 0x0000_0080_0000_0000 | 0x0000_FFFF_FFFF_FFFF | 0x000F_FFFF_FFFF_FFFF |
| Kernel PAGE_OFFSET | 0xFFFFFF8000000000 | 0xFFFF000000000000 | 0xFFF0000000000000 |
| T0SZ/T1SZ | 25 | 16 | 12 |
| Page table levels (4KB) | 3 | 4 | 5 |
| Total VA | 512 GB | 256 TB | 4 PB |
| Granule requirement | 4/16/64KB | 4/16/64KB | 64KB only |
| ARMv8 version | Any | Any | ARMv8.2 (LPA) |
