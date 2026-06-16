# Translation Granule Sizes: 4KB, 16KB, 64KB Tradeoffs

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

The translation granule defines the minimum page size in ARM64. It determines:
1. The size of leaf-level TLB entries (the smallest unit of mapping)
2. The structure of every page table level (how many bits are indexed per level)
3. The total number of page table levels required
4. The TLB coverage per entry (a 64KB page entry covers 16× more VA than a 4KB entry)

ARM64 supports three granule sizes: **4KB**, **16KB**, and **64KB**. The choice is made at boot via `TCR_EL1.TG0` (user) and `TCR_EL1.TG1` (kernel) and must match the granule the kernel was compiled for.

Different hardware may support different subsets:
```c
// ID_AA64MMFR0_EL1:
// TGran4  [31:28]: 0b0000 = 4KB supported, 0b1111 = not supported
// TGran16 [23:20]: 0b0001 = 16KB supported, 0b0000 = not supported
// TGran64 [27:24]: 0b0000 = 64KB supported, 0b1111 = not supported
```

---

## 2. 4KB Granule — The Standard Choice

### Properties

```
Page size:        4,096 bytes (2^12)
Page offset bits: 12 (VA[11:0])
Page table entry: 8 bytes (64-bit descriptor)
Entries per page table: 4096 / 8 = 512 entries (2^9)
Bits per level:   9 bits (indices 512 entries)
```

### Level Structure (48-bit VA)

```
VA[47:39]  L0 (PGD): 9 bits → 512 entries, each points to L1 table
VA[38:30]  L1 (PUD): 9 bits → 512 entries, each points to L2 table OR 1 GB block
VA[29:21]  L2 (PMD): 9 bits → 512 entries, each points to L3 table OR 2 MB block
VA[20:12]  L3 (PTE): 9 bits → 512 entries, each points to 4 KB page
VA[11:0]   Page offset: 12 bits
Total:     9+9+9+9+12 = 48 bits ✓
```

### Memory Overhead

```
Full 512-entry table: 512 × 8 = 4,096 bytes = exactly 1 page
PGD for one process: 1 page = 4 KB (maps 512 GB per entry × 512 = 256 TB)
Full 4-level walk for a 4 KB mapping: 4 × 4 KB = 16 KB of page table memory
```

### Linux ARM64 Default

4KB is the most common choice — nearly all ARM64 Linux systems use 4KB pages. It provides:
- Minimum memory waste per allocation
- Maximum sharing granularity (file page cache at 4KB)
- Compatibility with most hardware accelerators (DMA alignment assumptions)

---

## 3. 16KB Granule — Apple Silicon Choice

### Properties

```
Page size:        16,384 bytes (2^14)
Page offset bits: 14 (VA[13:0])
Page table entry: 8 bytes
Entries per page table: 16384 / 8 = 2,048 entries (2^11)
Bits per level:   11 bits (indices 2048 entries)
```

### Level Structure (48-bit VA)

```
VA[47:36]  L0 (PGD): 11 bits → 2048 entries (points to L1)
            Actually: ARM64 uses CONCATENATED tables for 16KB granule
            
Corrected layout (ARM64 specific):
VA[47:47]  L0: 1 bit  → 2 entries (concatenated PGD)
VA[46:36]  L1: 11 bits → 2048 entries (each → L2 or 64 GB block)
VA[35:25]  L2: 11 bits → 2048 entries (each → L3 or 32 MB block)
VA[24:14]  L3: 11 bits → 2048 entries (each → 16 KB page)
VA[13:0]   Page offset: 14 bits
Total: 1+11+11+11+14 = 48 bits ✓

Alternative (36-bit VA with 16KB):
T0SZ=28 → VA = 36 bits:
VA[35:25] L1: 11 bits → 2048 entries
VA[24:14] L2: 11 bits → 2048 entries (leaf)
VA[13:0]  Page offset
Total: 11+11+14 = 36 bits ✓ (3 levels)
```

### Why Apple Uses 16KB

Apple Silicon (M1/M2/M3) uses 16KB pages:
- Fewer TLB entries needed for same VA coverage (16KB entry = 4× coverage of 4KB)
- Reduced TLB pressure → less TLB misses on large allocations
- Better alignment for typical iOS/macOS allocation sizes
- Larger page tables per level (2048 entries vs 512) → L1 TLB hit more likely

**Compatibility issue**: Linux applications assume 4KB pages. Running a 4KB-compiled application on 16KB-page Linux requires the applications to handle 16KB alignment (`mmap` returns 16KB-aligned addresses). Most applications work fine; some low-level code (e.g., `mmap` with `MAP_FIXED` at 4KB boundaries) may have issues.

---

## 4. 64KB Granule — Server/HPC Choice

### Properties

```
Page size:        65,536 bytes (2^16)
Page offset bits: 16 (VA[15:0])
Page table entry: 8 bytes
Entries per page table: 65536 / 8 = 8,192 entries (2^13)
Bits per level:   13 bits (indices 8192 entries)
```

### Level Structure (48-bit VA)

```
VA[47:42]  L0: 6 bits  → 64 entries (concatenated, partial level)
VA[41:29]  L1: 13 bits → 8192 entries (each → L2 or 512 GB block)
VA[28:16]  L2: 13 bits → 8192 entries (each → 64 KB page or 512 MB block)
VA[15:0]   Page offset: 16 bits
Total: 6+13+13+16 = 48 bits ✓ (3 levels + partial L0)

Or for 42-bit VA (T0SZ=22):
VA[41:29] L1: 13 bits
VA[28:16] L2: 13 bits  
VA[15:0]  Page offset
= 13+13+16 = 42 bits ✓ (2 levels only)
```

### Advantages of 64KB

1. **Fewer TLB entries needed**: Each 64KB TLB entry covers 16× more VA than 4KB.
2. **Fewer page table levels**: Often only 2-3 levels needed for typical VA ranges.
3. **LPA support**: Required for ARMv8.2 52-bit PA extension (LPA).
4. **DMA efficiency**: Many server peripherals transfer in 64KB+ chunks; perfect alignment.
5. **Huge page as 2-level block**: A 2-level block descriptor maps 512 MB (vs 2 MB with 4KB granule) — extremely useful for server workloads with large NUMA memory pools.

### Disadvantages of 64KB

1. **Memory waste**: Every allocation rounded up to 64KB minimum → huge waste for small files/structs.
2. **Page cache inefficiency**: File pages are 64KB each → reading 1 byte loads 64KB from disk.
3. **Incompatibility**: Many applications assume 4KB pages. Libraries, interpreters, JVMs may misbehave.
4. **Not universally supported**: Not all ARM64 hardware supports 64KB granule (check `ID_AA64MMFR0_EL1.TGran64`).

---

## 5. Granule Comparison Table

```
Granule │ Page Size │ Levels  │ Block sizes     │ TLB Entry VA │ Table Size
────────┼───────────┼─────────┼─────────────────┼──────────────┼──────────
4KB     │ 4,096 B   │ 4       │ 2 MB, 1 GB      │ 4 KB         │ 4 KB
16KB    │ 16,384 B  │ 4       │ 32 MB, 64 GB    │ 16 KB        │ 16 KB
64KB    │ 65,536 B  │ 2-3     │ 512 MB, 512 GB  │ 64 KB        │ 64 KB
```

---

## 6. Linux Kernel Configuration

```makefile
# Kernel config options:
CONFIG_ARM64_4K_PAGES=y    # 4KB page, standard
CONFIG_ARM64_16K_PAGES=y   # 16KB page, Apple-compatible
CONFIG_ARM64_64K_PAGES=y   # 64KB page, server/HPC

# Only one can be active at a time — compile-time choice
# Cannot change granule at runtime (system restart required)

# VA bits typically:
# 4KB  → 48-bit VA (4 levels)
# 16KB → 48-bit VA (4 levels, but 2048 entries per table)
# 64KB → 48-bit VA (3 levels, 8192 entries per table)
```

---

## 7. Page Table Size per Granule

```
For 4KB granule, 48-bit VA, full user process (worst case):
  PGD: 512 entries × 8B = 4 KB
  Each PGD entry → PUD: 512 × 4 KB = 2 MB
  Each PUD entry → PMD: 512 × 4 KB = 2 MB  
  Each PMD entry → PTE: 512 × 4 KB = 2 MB
  Worst case (fully mapped 256 TB process): enormous — rarely happens

Typical process (1 GB mapped):
  PGD: 4 KB (all allocated, most entries invalid)
  PUD: 4 KB (1 entry used for the 1 GB region)
  PMD: 4 KB × 512 = 2 MB (for 1 GB worth of 2MB slabs)
  PTE: 4 KB × 512 × 512 = 1 GB of page tables (worst case for 1GB of 4KB pages)

In practice: use huge pages at PMD level = just PMD descriptor, no PTE needed
```

---

## 8. TLB Coverage per Granule

```
Single TLB entry coverage:
  4KB granule leaf:   4 KB → 1 TLB entry for 4 KB
  16KB granule leaf:  16 KB → 1 TLB entry for 16 KB (4× coverage)
  64KB granule leaf:  64 KB → 1 TLB entry for 64 KB (16× coverage)

L2 block coverage (useful for huge pages):
  4KB granule L2 block:  2 MB (512 × 4KB)
  16KB granule L2 block: 32 MB (2048 × 16KB)
  64KB granule L2 block: 512 MB (8192 × 64KB)

Impact on TLB efficiency for 1 GB process mapping:
  4KB: needs 262,144 TLB entries (or 512 huge page entries for 2MB blocks)
  16KB: needs 65,536 TLB entries (or 32 huge page entries for 32MB blocks)
  64KB: needs 16,384 TLB entries (or 2 huge page entries for 512MB blocks)
```

---

## 9. Interview Questions & Answers

**Q1: Why does Linux default to 4KB pages instead of 64KB?**

4KB pages minimize memory waste — a small file requires only one 4KB page in cache. With 64KB pages, reading a 1-byte file still loads 64KB into the page cache. 4KB also provides finer-grained protection (each 4KB region can have independent permissions), better sharing (shared libraries mapped at 4KB granularity with precise CoW), and compatibility with x86 assumptions in ported software. The tradeoff is more TLB pressure (4× more TLB entries than 16KB, 16× more than 64KB), but ARM64's TLBs are large enough to handle typical workloads.

**Q2: Why does Apple Silicon use 16KB pages?**

Apple's workloads (iOS, macOS apps) tend to have large allocation sizes (images, audio buffers, GPU resources). 16KB pages reduce TLB miss rate for these patterns. 16KB also allows 2-level translation for typical iOS virtual address ranges (36-bit VA was sufficient), reducing page table walk latency. The performance benefit outweighs the memory overhead for Apple's workloads. Importantly, Apple's runtime and OS are designed around 16KB — they don't have the 4KB compatibility burden that Linux ARM64 carries.

**Q3: How does the granule choice affect page table memory overhead?**

A 4KB page table has 512 entries × 8 bytes = 4KB (perfectly one page). A 16KB page table has 2048 entries × 8 bytes = 16KB (one page). A 64KB page table has 8192 entries × 8 bytes = 64KB. For a process mapping 1 GB of memory with 4KB pages at the leaf level: ~256 × 4KB tables = ~1 MB of page tables just for PTE level. With 64KB pages: ~16 × 64KB tables = ~1 MB page tables but each entry maps 16× more memory. So for densely-mapped processes, page table overhead is similar; the advantage of large granules shows in fewer levels needed.

---

## 10. Quick Reference

| Parameter | 4KB | 16KB | 64KB |
|---|---|---|---|
| `CONFIG_` | `ARM64_4K_PAGES` | `ARM64_16K_PAGES` | `ARM64_64K_PAGES` |
| `TCR_EL1.TG0` | 0b00 | 0b10 | 0b01 |
| `TCR_EL1.TG1` | 0b10 | 0b01 | 0b11 |
| VA levels (48-bit) | 4 | 4 | 3 |
| L2 block size | 2 MB | 32 MB | 512 MB |
| L1 block size | 1 GB | 64 GB | 512 GB |
| LPA support | No | No | Yes |
| Table entries/page | 512 | 2048 | 8192 |
| Used by | Most Linux | Apple Silicon | Some HPC/server |
