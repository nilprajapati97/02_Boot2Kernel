# ARM64 VA Split and Translation Regime

## 1. The Two-Region VA Model

ARM64 divides the 64-bit virtual address space into **two halves**:

```
0xFFFF_FFFF_FFFF_FFFF  ┐
                        │   Upper half — TTBR1_EL1 (Kernel Space)
0xFFFF_0000_0000_0000  ┘   256 TB (with 48-bit VA)

         ── unmapped gap ──  (causes fault if accessed)

0x0000_FFFF_FFFF_FFFF  ┐
                        │   Lower half — TTBR0_EL1 (User Space)
0x0000_0000_0000_0000  ┘   256 TB (with 48-bit VA)
```

This is controlled by **`TCR_EL1.T0SZ`** (user range size) and
**`TCR_EL1.T1SZ`** (kernel range size).

---

## 2. VA Routing Rule

The CPU uses bits `[63:48]` to select the TTBR:

```
VA[63:48] == 0x0000  →  TTBR0_EL1  (user space)
VA[63:48] == 0xFFFF  →  TTBR1_EL1  (kernel space)
VA[63:48] == anything else → Translation Fault
```

This means the **canonical VA check** is implicit:
addresses with bits `[63:49]` not all 0 or not all 1 are invalid.

---

## 3. `TCR_EL1.T1SZ` and `TCR_EL1.T0SZ` Explained

`TxSZ` is the **size offset**. The formula for VA bit width is:

$$\text{VA bits} = 64 - \text{TxSZ}$$

| TxSZ | VA bits | VA range size |
|------|---------|---------------|
| 16   | 48      | 256 TB |
| 20   | 44      | 16 TB |
| 25   | 39      | 512 GB |
| 32   | 32      | 4 GB |

**Default ARM64 Linux: `T0SZ = T1SZ = 16` → 48-bit VA.**

With `CONFIG_ARM64_VA_BITS_52` (ARMv8.2-LPA) on capable hardware:
`T0SZ = T1SZ = 12` → 52-bit VA → 4 PB range.

---

## 4. Full VA Map (48-bit configuration)

```
Virtual Address Space — ARM64 Linux (48-bit, 4KB pages)

0xFFFF_FFFF_FFFF_FFFF  ──────────────────────────────────
0xFFFF_FFFE_0000_0000    fixmap region (compile-time VAs)
0xFFFF_FFFD_FFFF_FFFF
                         kasan shadow region
0xFFFF_FE00_0000_0000
                         vmalloc / ioremap region
0xFFFF_8000_0000_0000
                         vmemmap (struct page array)
0xFFFF_7FFF_FFFF_FFFF
                         modules region
0xFFFF_800_0080_0000     KIMAGE_VADDR (kernel image start)
                         .text, .rodata, .data, .bss
0xFFFF_0000_0000_0000  ──────────────────────────────────

           ← HOLE (unmapped; fault if accessed) →

0x0000_FFFF_FFFF_FFFF  ──────────────────────────────────
                         user stack (grows down)
                         ...
                         user heap (grows up)
                         user BSS
                         user data
                         user text
0x0000_0000_0000_0000  ──────────────────────────────────
```

---

## 5. Page Table Walk — 4-Level (48-bit VA, 4KB pages)

With 48-bit VA and 4KB granule, ARM64 uses a **4-level** page table walk:

```
Virtual Address breakdown (48-bit):

  Bits [47:39]  →  PGD index   (9 bits → 512 entries)
  Bits [38:30]  →  PUD index   (9 bits → 512 entries)
  Bits [29:21]  →  PMD index   (9 bits → 512 entries)
  Bits [20:12]  →  PTE index   (9 bits → 512 entries)
  Bits [11:0]   →  Page offset (12 bits → 4096 bytes)
```

### Walk Diagram

```
TTBR1_EL1 ──► PGD (4KB, 512×8B entries)
               │
               │  [VA[47:39]]
               ▼
              PUD (4KB, 512×8B entries)       or 1GB block descriptor
               │
               │  [VA[38:30]]
               ▼
              PMD (4KB, 512×8B entries)       or 2MB block descriptor
               │
               │  [VA[29:21]]
               ▼
              PTE (4KB, 512×8B entries)       or 4KB page descriptor
               │
               │  [VA[20:12]]
               ▼
              Physical Page
               │
               │  + VA[11:0] (page offset)
               ▼
              Physical Address
```

---

## 6. Block vs Page Descriptors (Huge Pages)

ARM64 supports "short-circuit" mappings at PUD and PMD level:

| Level | Block Size | Use case |
|---|---|---|
| PUD block | 1 GB | Large device memory, initial kernel map |
| PMD block | 2 MB | Kernel text/data, large contiguous allocations |
| PTE page  | 4 KB | Fine-grained user pages |

In `__create_page_tables`, the kernel image is typically mapped using
**2MB PMD blocks** for efficiency.

---

## 7. ASID — Address Space Identifier

ASIDs prevent TLB flushing on every context switch.

```
TCR_EL1.AS = 0  →  8-bit ASID  (256 unique address spaces)
TCR_EL1.AS = 1  →  16-bit ASID (65536 unique address spaces)
```

The ASID is stored in bits `[63:48]` of the TTBR0 register:

```
TTBR0_EL1 [63:48] = ASID
TTBR0_EL1 [47:0]  = PGD base address
```

When the scheduler context-switches:
```c
// arch/arm64/include/asm/mmu_context.h
cpu_switch_mm(next->pgd, next);
    → write new TTBR0_EL1 with new ASID + new PGD
    → TLB entries tagged with old ASID remain but are ignored
```

---

## 8. Tagged Addresses (TBI — Top Byte Ignore)

ARM64 optionally supports **Top Byte Ignore**:

```
TCR_EL1.TBI0 = 1  →  bits [63:56] of TTBR0 addresses are ignored by hardware
TCR_EL1.TBI1 = 1  →  bits [63:56] of TTBR1 addresses are ignored by hardware
```

Linux uses `TBI0 = 1` to support **pointer tagging** (used by HWASAN,
MTE, and userspace pointer authentication).

The hardware treats `0x4200DEAD_DEADBEEF` the same as `0x00000DEAD_DEADBEEF`
for translation purposes when `TBI0` is set.

---

## 9. Translation Fault Handling

When an access hits the unmapped "canonical hole":

```
MMU raises: Translation Fault (ESR_EL1.EC = 0b100100 or 0b100101)
Kernel handles in: do_translation_fault() → do_page_fault()
```

For kernel accesses outside mapped regions, this results in an **Oops**
(kernel bug) or `BUG()` call.

---

## 10. Summary Table

| Parameter | Value | Notes |
|---|---|---|
| VA bits | 48 (default) / 52 (LPA) | Set by `CONFIG_ARM64_VA_BITS` |
| T0SZ | 16 (48-bit VA) | User range control |
| T1SZ | 16 (48-bit VA) | Kernel range control |
| Page granule | 4KB / 16KB / 64KB | Set by `CONFIG_ARM64_PAGE_SHIFT` |
| Page table levels | 4 (4KB, 48-bit) | PGD → PUD → PMD → PTE |
| Huge pages | 2MB (PMD), 1GB (PUD) | Block descriptors |
| ASID width | 8-bit (default) / 16-bit | `CONFIG_ARM64_16K_PAGES` affects |
| TTBR1 (kernel) | `swapper_pg_dir` | Never changes after `paging_init()` |
| TTBR0 (user) | Per-process `pgd` | Changed on every context switch |
