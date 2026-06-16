# Physical-to-Virtual Jump — CPU Internals at `br x8`

**Event:** The CPU transitions from executing at a PA (identity-mapped) to a kernel VA  
**Instruction:** `br x8` where x8 = kernel VA of `__primary_switched`

---

## 0. The Two Worlds

Before `br x8`:
```
PC in identity map:  VA = PA = 0x0000_0000_4000_1238  (TTBR0 range, VA[47]=0)
```

After `br x8`:
```
PC in kernel space:  VA = 0xFFFF_8000_xxxx_xxxx  (TTBR1 range, VA[47]=1)
```

The physical memory accessed is different for instruction fetches:
- Before: TTBR0 translates `0x4000_12xx` → same PA (identity)
- After: TTBR1 translates `0xFFFF_8000_xxxx_xxxx` → actual PA of kernel text

---

## 1. TTBR Selection Rule — Hardware

The ARM64 MMU selects between TTBR0 and TTBR1 based on the **top bits** of the VA:

```
TCR_EL1.TG0 = 0b00 (4KB granule for TTBR0)
TCR_EL1.TG1 = 0b10 (4KB granule for TTBR1)
TCR_EL1.T0SZ = 16  (TTBR0 covers VA[47:0] when VA[63:48] = 0x0000)
TCR_EL1.T1SZ = 16  (TTBR1 covers VA[47:0] when VA[63:48] = 0xFFFF)

Selection:
  VA[63:48] = 0x0000 → TTBR0_EL1 (user space)
  VA[63:48] = 0xFFFF → TTBR1_EL1 (kernel space)
  Anything else → Translation fault (VA hole)
```

For `x8 = 0xFFFF_8000_xxxx_xxxx`:
- VA[63:48] = 0xFFFF → TTBR1_EL1 selected
- TTBR1_EL1 = PA of `swapper_pg_dir`

---

## 2. The Page Table Walk at `br x8`

When the CPU fetches the first instruction at the kernel VA, it performs a PTW
through `swapper_pg_dir`:

```
VA = 0xFFFF_8000_1234_5678 (example)

Bit decomposition (48-bit VA, 4KB granule, 4-level tables):
  VA[47:39] = bits 47-39 → PGD index (9 bits, 512 entries)
  VA[38:30] = bits 38-30 → PUD index (9 bits, 512 entries)
  VA[29:21] = bits 29-21 → PMD index (9 bits, 512 entries)
  VA[20:0]  = bits 20-0  → offset within 2MB block

Walk:
  1. TTBR1_EL1[47:1] = PA of swapper_pg_dir (with ASID=0 in bits 63:48)
  2. PGD entry = swapper_pg_dir[VA[47:39]] → PUD base PA
  3. PUD entry = PUD[VA[38:30]] → PMD base PA (or 1GB block)
  4. PMD entry = PMD[VA[29:21]] → 2MB block descriptor:
       bits[1:0] = 0b01 (block entry)
       bits[47:21] = PA[47:21] (2MB-aligned PA of kernel text block)
       AF=1 (Access Flag, already set during mapping)
       PXN=0 (Privileged eXecute Never = 0, execute allowed)
       UXN=1 (Unprivileged eXecute Never = 1, user can't execute)
       AP[2:1] = 0b00 (read-write, EL1 only)
  5. PA = PMD.PA[47:21] : VA[20:0]
       = 2MB block base + offset within block
```

The `swapper_pg_dir` PMD entries for the kernel text section have `PXN=0`,
meaning instruction fetches are permitted.

---

## 3. The TLB Fill After `br x8`

The TLB is warm for TTBR0 entries (identity map was used actively), but cold
for TTBR1 kernel VA entries (first use).

First instruction fetch:
1. TLB miss for `0xFFFF_8000_xxxx_xxxx`
2. Hardware PTW through `swapper_pg_dir`
3. TLB fill with the result
4. Subsequent fetches in the same 2MB block → TLB hit

The TLB entry contains:
```
ASID: 0 (global entry, ASID-independent — kernel space)
VA:   0xFFFF_8000_xxxx_x000 (page-aligned portion)
PA:   actual PA of that page
attrs: PXN=0, AF=1, shareability, cacheability etc.
```

---

## 4. I-Cache Miss and Branch Target

After the TLB fill, the CPU fetches the instruction from physical memory:
- PA computed from PTW result
- L1 I-cache lookup: miss (first fetch of this PA after MMU enable)
- L2/L3 lookup or DRAM fetch
- Instruction bytes returned, L1 I-cache filled

The instruction at `__primary_switched` (with BTI enabled):
```asm
hint    #36    // BTI j instruction (opcode: 0xD503245F)
```

This is the first instruction executed in the kernel virtual address space.

---

## 5. Why The Identity Map Is No Longer the Active Code Path

After `br x8`, the PC is in TTBR1 territory. The CPU never "returns" to the
identity map VA (`0x4000_xxxx`). However, the identity map is still VALID in
TTBR0 at this point.

The identity map is torn down later in:
```c
// arch/arm64/mm/mmu.c
static void __init cpu_replace_ttbr1(pgd_t *pgdp, phys_addr_t ttbr1)
{
    // Replace swapper_pg_dir with the final init_mm page tables
    // (after the linear map and all other regions are mapped)
}
```

And the identity map pages are freed in:
```c
// arch/arm64/mm/init.c
void __init free_initmem(void)
{
    // ...frees __init sections including idmap_pg_dir
}
```

Until that happens, the identity map is an unused but valid entry in TTBR0.
Any accidental branch to a low VA would still execute via the identity map
(potentially harmful, but not immediately fatal if the PA is valid).

---

## 6. CPU Pipeline at `br x8` Execution

High-level view of an out-of-order ARM64 pipeline at the branch:

```
Fetch stage:
  Fetching instructions at PA 0x4000_12xx (identity map)
  Fetch unit predicts: "no branch" (or mispredicts indirect target)

Decode stage:
  Decodes `br x8` as an indirect branch

Execute stage:
  Reads x8 = 0xFFFF_8000_xxxx_xxxx
  Computes new PC = x8

Branch resolution:
  New PC broadcast to frontend
  All speculatively fetched instructions after `br x8` at old PC are squashed
  
Re-fetch from new PC:
  Instruction fetch restarts at VA 0xFFFF_8000_xxxx_xxxx
  TLB lookup (miss) → PTW → TLB fill → cache miss → DRAM fetch
  `hint #36` (BTI j) instruction arrives and executes
  PSTATE.BTYPE checked: was 0b10 (indirect jump expected), BTI j satisfies it
  PSTATE.BTYPE reset to 0b00
  Execution continues in __primary_switched
```

---

## 7. Summary: The PA→VA Transition

| Moment | PC Value | MMU | TTBR | Notes |
|---|---|---|---|---|
| Before `br x8` | `0x4000_1238` (PA=VA) | On | TTBR0=idmap, TTBR1=swapper | Running in identity window |
| At `br x8` | `0x4000_1238` → executing | On | Same | Last instruction at PA world |
| After `br x8` | `0xFFFF_8000_xxxx_xxxx` | On | TTBR1 selected | First instruction in VA world |
| `__primary_switched` body | VA throughout | On | TTBR1=swapper | Kernel VA space |
| `start_kernel` | VA throughout | On | TTBR1=final | Full kernel running |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The BR Xn instruction in ARMv8-A is an unconditional indirect branch: it sets PC = Xn. Unlike B (immediate) or BL (branch-with-link), BR does not use the PC-relative encoding and can reach any 64-bit address. The CPU's branch predictor can predict indirect branches using the indirect branch predictor (IBP) or indirect branch target buffer. After the MMU enable, the very first BR to a kernel VA address triggers a TLB miss (if the kernel VA is not yet in the TLB), which initiates a page-table walk using TTBR1_EL1. The walk succeeds if the kernel page tables have been correctly set up by __pi_early_map_kernel.

### Kernel Perspective (Linux ARM64)
In __primary_switch, the sequence:
  ldr x8, =primary_switched   // load VA of target function
  br  x8                       // jump to kernel VA
This is the point-of-no-return: the CPU jumps from the identity-mapped PA region to the kernel VA. After BR x8, x30 (link register) still holds the identity-mapped return address from the original caller, but it is never used again. The CPU is now fully in kernel VA space. Spectre-v2 mitigations require that indirect branches be protected: on patched kernels, BR x8 may be replaced by a retpoline or enhanced IBRS sequence.

### Memory Perspective (ARMv8 Memory Model)
The BR x8 instruction that transitions the CPU from PA to VA is a memory system event: it changes the instruction fetch address from an identity-mapped PA (low VA) to a high kernel VA. At this point, TTBR0_EL1 still points to the identity map (it will be cleared later when init_task's mm is set to NULL). TTBR1_EL1 covers the target VA. The TLB may not yet have an entry for the target VA; the first access causes a hardware page-table walk. If the page-table walk finds a valid entry (Normal, Execute permission, correct PA), the CPU populates the TLB and continues. This walk is the first use of the kernel page tables built by __pi_early_map_kernel.