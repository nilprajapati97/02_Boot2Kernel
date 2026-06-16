# Canonical Address Rule and the Hole Between User and Kernel VA

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

In AArch64, not all 64-bit values are valid virtual addresses. A valid VA must follow the **canonical address rule**: the upper bits (above the implemented VA width) must be a sign-extension of bit[VA_BITS-1].

This creates a **VA hole** — a large range of addresses that are architecturally invalid. The hole separates user space (low VAs) from kernel space (high VAs), providing:
1. A clear boundary between user and kernel VAs
2. Protection against pointer corruption bugs (corrupted pointers likely fall in the hole)
3. A "no man's land" that cannot be accidentally mapped by either user or kernel code

---

## 2. Canonical Address Rule: Exact Definition

For a given VA width of N bits:
```
Bits [63:N] must equal bit [N-1] (sign extension)

Case 1: bit[N-1] = 0 → all bits [63:N] must be 0x00...0
Case 2: bit[N-1] = 1 → all bits [63:N] must be 0xFF...F
```

Any VA violating this rule is **non-canonical** → Translation Fault, Level 0.

---

## 3. VA Hole for Different VA Sizes

### 48-bit VA (most common)

```
N = 48, bit[47] is the sign bit:

Valid user space:    0x0000_0000_0000_0000 → 0x0000_FFFF_FFFF_FFFF
   (bit[47]=0, bits[63:48] = 0x0000)

VA HOLE (invalid):   0x0001_0000_0000_0000 → 0xFFFE_FFFF_FFFF_FFFF
   (any value where bits[63:48] ≠ 0x0000 and ≠ 0xFFFF)

Valid kernel space:  0xFFFF_0000_0000_0000 → 0xFFFF_FFFF_FFFF_FFFF
   (bit[47]=1, bits[63:48] = 0xFFFF)

Hole size: 0xFFFE_FFFF_FFFF_FFFF - 0x0001_0000_0000_0000 + 1
         = ~128 PB (petabytes) of invalid VA space
```

### 39-bit VA (embedded/small systems)

```
N = 39, bit[38] is the sign bit:

Valid user:   0x0000_0000_0000_0000 → 0x0000_007F_FFFF_FFFF  (512 GB)
VA HOLE:      0x0000_0080_0000_0000 → 0xFFFF_FF80_0000_0000
Valid kernel: 0xFFFF_FF80_0000_0000 → 0xFFFF_FFFF_FFFF_FFFF  (512 GB)
```

### 52-bit VA (LPA)

```
N = 52, bit[51] is the sign bit:

Valid user:   0x0000_0000_0000_0000 → 0x000F_FFFF_FFFF_FFFF  (4 PB)
VA HOLE:      0x0010_0000_0000_0000 → 0xFFF0_FFFF_FFFF_FFFF
Valid kernel: 0xFFF0_0000_0000_0000 → 0xFFFF_FFFF_FFFF_FFFF  (4 PB)
```

---

## 4. What Happens on a Non-Canonical Access?

When code accesses a non-canonical address (e.g., `LDR X0, [X1]` where X1 = 0x0001_1234_5678_9000):

1. CPU checks VA[63:48] before starting the page table walk.
2. Finds VA[63:48] = 0x0001 ≠ 0x0000 and ≠ 0xFFFF → non-canonical.
3. Raises a Synchronous Exception:
   - `ESR_EL1.EC = 0x21` (Data Abort from EL0) or `0x25` (Data Abort from EL1)
   - `ESR_EL1.DFSC = 0b000000` (Translation Fault, Level 0)
   - `FAR_EL1` = the faulting address (0x0001_1234_5678_9000)
4. Linux kernel handles it:
   - From EL0: delivers SIGSEGV to the process
   - From EL1: kernel oops/panic

---

## 5. Linux Kernel VA Layout (48-bit VA, 4KB)

The kernel uses the TTBR1 region (0xFFFF...) for all kernel virtual memory:

```
0xFFFF_FFFF_FFFF_FFFF  ← top of VA space
   Kernel modules          (MODULES_VADDR to MODULES_END)
   ~128 MB
0xFFFF_8000_0000_0000
   vmalloc / ioremap       (VMALLOC_START to VMALLOC_END)
   ~128 TB range
   vmemmap                 (sparse memory map for struct page)
   PCI I/O space
   Fixed mappings          (fixmap, early_ioremap)
0xFFFF_0000_0000_0000  ← PAGE_OFFSET (kernel linear map base)
   Direct-mapped RAM       (linear map)
   All physical RAM mapped here contiguously
   PA 0x0 → VA PAGE_OFFSET
   PA 0x1000 → VA PAGE_OFFSET + 0x1000
   ...
0xFFFF_0000_0000_0000  ← start of kernel VA
```

```c
// arch/arm64/include/asm/memory.h (48-bit VA example):
#define PAGE_OFFSET     UL(0xffff000000000000)  // Linear map start
#define KIMAGE_VADDR    (MODULES_END)            // Kernel image VA
#define MODULES_VADDR   (KIMAGE_VADDR - SZ_128M) // Modules
#define VMALLOC_START   (MODULES_VADDR - SZ_256M) // vmalloc
#define VMALLOC_END     (PAGE_OFFSET - SZ_256M - SZ_4K)
```

---

## 6. User Space VA Layout

The user VA space (TTBR0 region) is divided by the OS through `mmap`, but conventionally:

```
0x0000_FFFF_FFFF_FFFF  ← top of user VA (for 48-bit VA)
   Stack                 (grows down from top)
   Stack gap
   mmap region           (shared libs, anonymous mmaps, file mappings)
   Heap                  (grows up via brk)
   BSS / data
   Text
0x0000_0000_0040_0000  ← ELF load address (typical)
0x0000_0000_0000_0000  ← zero page (unmapped — NULL pointer guard)
```

The Linux mmap allocator starts from `TASK_UNMAPPED_BASE` and grows:
```c
// arch/arm64/include/asm/processor.h
#define TASK_SIZE_64    (UL(1) << VA_BITS)   // = 2^48 for 48-bit VA
#define TASK_UNMAPPED_BASE  (PAGE_ALIGN(TASK_SIZE / 4))
```

---

## 7. Why the Hole Helps Security

**Bug detection**: If a pointer is corrupted (e.g., by a buffer overflow), the corrupted value is likely to fall in the hole (probability: 128 PB hole / 512 TB valid = ~256× more likely to be in hole than valid space). The resulting Translation Fault is caught before any actual data corruption.

**Meltdown prevention**: Without KPTI, kernel VAs are not mapped in user space page tables. A user process cannot access kernel memory because kernel addresses (0xFFFF...) are handled by TTBR1, and TTBR1 contains the kernel's protected page tables. Even speculative access to kernel VAs from EL0 code requires a valid TTBR1 entry — which the user process doesn't have permission to reach (access permission bits enforce EL1-only).

**KASLR**: Kernel Address Space Layout Randomization places the kernel image at a random offset within the kernel VA region. The hole ensures that user space pointers can never accidentally overlap with kernel VAs even with randomization.

---

## 8. Interview Questions & Answers

**Q1: What is the canonical address hole and why does it exist?**

The canonical address hole is the range of VAs where the upper bits don't form a valid sign extension of bit[VA_BITS-1]. For 48-bit VA, this is 0x0001_0000_0000_0000 to 0xFFFE_FFFF_FFFF_FFFF — a ~16 million TB gap. It exists because ARM64 only uses N bits for VA (e.g., 48), so bits [63:N] must match bit[N-1] (be canonical). The hole cleanly separates user (low) and kernel (high) VA spaces. Any pointer that "crosses" the hole (via bug or corruption) is immediately caught as a Translation Fault before any memory is accessed.

**Q2: Can you explain why a NULL pointer dereference (`*((int*)0)`) faults?**

Address 0x0 is in the user VA space but is deliberately left unmapped (no VMA covers address 0 in any normal process). The MMU walks the page table looking for a mapping. Finding no valid PTE, it raises a Translation Fault (DFSC = 0b000100, Level 2 or Level 3 depending on where the walk fails). Linux receives this as a data abort and sends SIGSEGV to the process. The bottom page (0x0 to 0xFFF) is never mapped — `mmap` refuses to map at address 0 unless `vm.mmap_min_addr=0` is explicitly set.

**Q3: How does the canonical hole differ between ARM64 and x86-64?**

On x86-64 with 48-bit VA, the canonical rule is identical: bits [63:48] must equal bit[47]. The hole is the same range. However, x86-64 with 5-level paging (57-bit VA) has a different hole: bits [63:57] must equal bit[56]. ARM64 with 52-bit VA (LPA) similarly changes the boundary. The fundamental concept is the same on both architectures — bits above the implemented VA width must be sign-extensions of the highest implemented bit.

---

## 9. Quick Reference

| Config | VA Bits | User Top | Hole | Kernel Bottom | Hole Size |
|---|---|---|---|---|---|
| 39-bit | 39 | 0x7FFFFFFFFF | 0x80...~0xFFFFFF7FFFFFFFFF | 0xFFFFFF8000000000 | ~512 TB |
| 48-bit | 48 | 0x0000FFFFFFFFFFFF | 0x0001...~0xFFFEFFFFFFFFFFFF | 0xFFFF000000000000 | ~128 PB |
| 52-bit | 52 | 0x000FFFFFFFFFFFFF | 0x0010...~0xFFFEFFFFFFFFFFFF | 0xFFF0000000000000 | ~4 PB |
