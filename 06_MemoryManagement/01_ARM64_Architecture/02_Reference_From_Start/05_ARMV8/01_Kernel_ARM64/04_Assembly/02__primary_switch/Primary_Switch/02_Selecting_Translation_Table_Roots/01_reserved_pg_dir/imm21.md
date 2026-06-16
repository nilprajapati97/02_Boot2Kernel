## `imm21` in `adrp` — Complete Deep Dive

---

### What `~0xFFF` Does (The Masking Operation)

```
0xFFF        = 0000 0000 0000 0000 0000 1111 1111 1111  (12 low bits = 1)
~0xFFF       = 1111 1111 1111 1111 1111 0000 0000 0000  (12 low bits = 0)

addr & ~0xFFF → clears bottom 12 bits → rounds DOWN to nearest 4KB page start
```

This is because a 4KB page is exactly $2^{12}$ bytes, so the **page offset** lives in bits[11:0] and the **page number** lives in bits[63:12].

---

### Why Two Separate `PAGE()` Operations?

```
PAGE(reserved_pg_dir) = reserved_pg_dir & ~0xFFF
```
→ "Which 4KB page does `reserved_pg_dir` live in?"

```
PAGE(adrp_instruction) = PC & ~0xFFF
```
→ "Which 4KB page is this `adrp` instruction executing from?"

```
imm21 = PAGE(reserved_pg_dir) - PAGE(adrp_instruction)
      = (difference in bytes, always a multiple of 4096)
      >> 12
      = "how many 4KB pages away is the target from the executing page"
```

Both addresses are page-aligned before subtracting, so the difference is **always a multiple of 4096**. Dividing by 4096 (i.e., `>> 12`) gives a clean integer — that integer is `imm21`.

---

### Concrete Numerical Example

Suppose:
| Symbol | Physical Address |
|---|---|
| `adrp` instruction | `0x4008_1350` |
| `reserved_pg_dir` | `0x4008_2000` |

```
PAGE(reserved_pg_dir) = 0x4008_2000 & ~0xFFF = 0x4008_2000  (already aligned)
PAGE(adrp_instruction) = 0x4008_1350 & ~0xFFF = 0x4008_1000

imm21 = (0x4008_2000 - 0x4008_1000) >> 12
      = 0x0000_1000 >> 12
      = 1
```

At runtime the CPU executes:
$$x1 = (PC\ \&\ \sim\texttt{0xFFF}) + (\text{sign\_extend}(\text{imm21}) \ll 12)$$
$$= \texttt{0x4008\_1000} + (1 \ll 12) = \texttt{0x4008\_1000} + \texttt{0x1000} = \texttt{0x4008\_2000} \checkmark$$

---

### Why Signed? Why 21 Bits?

`imm21` is **2's complement signed** because `reserved_pg_dir` could be **either before or after** the `adrp` instruction in memory:

| Direction | Value of imm21 | Meaning |
|---|---|---|
| Target is **after** PC | Positive (N > 0) | `reserved_pg_dir` is N pages forward |
| Target is **before** PC | Negative (N < 0) | `reserved_pg_dir` is N pages backward |

With 21 signed bits (2's complement):

$$\text{Range} = -2^{20}\ \text{to}\ +2^{20}-1 = \pm 1{,}048{,}575\ \text{pages}$$

$$\pm 1{,}048{,}575 \times 4096\ \text{bytes} = \pm 4\ \text{GB from the PC page}$$

The entire ARM64 kernel image is well within ±4GB, so `adrp` can reach any kernel symbol with a single instruction.

---

### How `imm21` Is Split Inside the 32-bit Instruction Word

The 21 bits are **not stored contiguously** in the instruction encoding — they're split into two fields:

```
Bit positions:  31  30 29  28-24  23──────────────5  4──0
                 1  immlo[1:0]  10000   immhi[18:0]   Rd[4:0]
```

| Field | Bits in opcode | Width | Content |
|---|---|---|---|
| `immhi` | [23:5] | 19 bits | Upper 19 bits of imm21 |
| `immlo` | [30:29] | 2 bits | Lower 2 bits of imm21 |

The CPU decoder reassembles them: `imm21 = immhi:immlo` (19 bits concatenated with 2 bits). This split is a quirk of the AArch64 fixed-width encoding to keep opcode fields regular.

---

### The Full Chain: Linker → Encoding → CPU

```
LINK TIME (ld):
  knows: address of adrp instruction  → PC_link
  knows: address of reserved_pg_dir   → target
  computes:
    raw_diff = (target & ~0xFFF) - (PC_link & ~0xFFF)   ← page difference in bytes
    imm21    = raw_diff >> 12                            ← page count (signed)
  encodes imm21 into bits [30:29] and [23:5] of the 32-bit instruction word

RUN TIME (CPU, MMU off):
  Step 1:  PC        = physical address of this instruction
  Step 2:  PC_page   = PC & ~0xFFF          (strip page offset)
  Step 3:  offset    = sign_extend(imm21) << 12   (scale back to bytes)
  Step 4:  x1        = PC_page + offset     (page-aligned result)
```

The `<< 12` on the CPU side is the inverse of the `>> 12` the linker did — they cancel out, leaving you with the exact byte address of the target page.

---

### Why `reserved_pg_dir` Is Always Exactly Page-Aligned

From the linker script:
```ld
reserved_pg_dir = .;   . += PAGE_SIZE;   // . is always page-aligned here
```

Because `. += PAGE_SIZE` advances the location counter by exactly 4096 bytes, and the linker script ensures alignment, `reserved_pg_dir` is always at `addr & ~0xFFF == addr` — meaning its page offset is zero. So `PAGE(reserved_pg_dir) == reserved_pg_dir` exactly, and `x1` after the `adrp` is the **exact physical address** of `reserved_pg_dir`, not just its page base.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses a multi-level page-table walk starting at the physical address stored in TTBR0_EL1 (low VA, user) or TTBR1_EL1 (high VA, kernel). With 4 KB granule and 48-bit VA (T0SZ/T1SZ=16): the walk has 4 levels: L0 (PGD), L1 (PUD), L2 (PMD), L3 (PTE). Each table entry is 8 bytes. A leaf entry (block or page descriptor) contains the PA, memory attributes (AttrIdx into MAIR_EL1), access permissions (AP bits), shareability, and XN/PXN bits. The hardware page-table walker is fully autonomous: on a TLB miss, it reads TTBR, walks the tables in memory, and populates the TLB entry without software intervention.

### Kernel Perspective (Linux ARM64)
Linux organizes the early boot page tables into:
- __idmap_pg_dir: identity map (VA==PA) covering the .idmap.text section, used during MMU enable.
- init_pg_dir / swapper_pg_dir: kernel page table covering the TTBR1_EL1 region.
__pi_early_map_kernel (arch/arm64/mm/mmu.c) allocates and populates these tables using fixmap and the early pgtable allocator before __enable_mmu is called. After start_kernel, the definitive kernel page tables are built by paging_init().

### Memory Perspective (ARMv8 Memory Model)
Each page-table entry encodes both the physical address and the memory type for that region. The ARMv8 memory model treats adjacent pages independently: two adjacent 4 KB pages can have different memory attributes (e.g., one Normal cacheable, one Device). Block entries (2 MB at L2, 1 GB at L1) allow the walker to terminate early, reducing TLB fill latency and the number of memory accesses per walk. The hardware guarantees that table walks are coherent with the data cache if the inner-shareable domain includes the page-table memory (which is true for all Linux ARM64 configurations).