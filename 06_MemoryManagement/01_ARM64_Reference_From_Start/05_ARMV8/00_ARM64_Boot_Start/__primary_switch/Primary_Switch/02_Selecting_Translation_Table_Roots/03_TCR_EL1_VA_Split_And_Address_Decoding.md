# TCR_EL1 VA Split and Address Decoding — How the CPU Decides TTBR0 vs TTBR1

**Relevant lines in `__primary_switch`:**
```asm
adrp    x1, reserved_pg_dir        // TTBR1 root
adrp    x2, __pi_init_idmap_pg_dir // TTBR0 root
bl      __enable_mmu
```

**Question this document answers:** When the MMU is on, given a virtual address
fetched by the CPU, how does the hardware decide whether to use TTBR0 or TTBR1?

---

## 0. The Two VA Regimes

AArch64 at EL1 has **two independent translation regimes**:

| Regime | Register | VA range | Used for |
|---|---|---|---|
| TTBR0_EL1 | `TTBR0_EL1` | Low (user) VAs — bits all 0s at top | User space + identity map at boot |
| TTBR1_EL1 | `TTBR1_EL1` | High (kernel) VAs — bits all 1s at top | Kernel virtual address space |

The decision is made purely on the **top bits of the virtual address**, without
looking at page tables at all. It is a simple hardware decode.

---

## 1. The Selection Algorithm

The ARM Architecture Reference Manual (ARM ARM D5.2.4) defines the selection rule:

### Step 1: Check Top Bits Against T0SZ

```
T0SZ = TCR_EL1[5:0]   (for 48-bit VA: T0SZ = 16)

top_zeros = count of leading zero bits in VA[63:0]

If top_zeros >= T0SZ:
    Use TTBR0_EL1
```

For T0SZ=16, a VA is in the TTBR0 range if its top 16 bits are all zero:

```
0x0000_xxxx_xxxx_xxxx  → VA[63:48] = 0x0000 → 16 leading zeros ≥ T0SZ=16 → TTBR0
```

### Step 2: Check Top Bits Against T1SZ

```
T1SZ = TCR_EL1[21:16]   (for 48-bit VA: T1SZ = 16)

top_ones = count of leading one bits in VA[63:0]

If top_ones >= T1SZ:
    Use TTBR1_EL1
```

For T1SZ=16, a VA is in the TTBR1 range if its top 16 bits are all one:

```
0xFFFF_xxxx_xxxx_xxxx  → VA[63:48] = 0xFFFF → 16 leading ones ≥ T1SZ=16 → TTBR1
```

### Step 3: Neither Matches → Translation Fault

```
0x1234_5678_9abc_def0  → Not all zeros at top, not all ones at top → FAULT
```

This is the VA **hole** — the region between user and kernel space that has
no mapping and immediately causes a Translation Fault.

---

## 2. Visual Diagram for 48-bit VA (T0SZ=T1SZ=16)

```
64-bit Virtual Address Space

Bit 63                                                      Bit 0
  ┌──────────────────────────────────────────────────────────────┐
  │ 0xFFFF_FFFF_FFFF_FFFF                                        │◄── TTBR1
  │ 0xFFFF_8000_0000_0000  ←── PAGE_OFFSET (linear map start)   │
  │     ┊                                                        │
  │ 0xFFFF_0000_0000_0000                                        │◄── TTBR1 boundary
  ├──────────────────────────────────────────────────────────────┤
  │          V  A     H  O  L  E    (Translation Fault)          │
  │ 0x0001_0000_0000_0000 to 0xFFFE_FFFF_FFFF_FFFF               │
  ├──────────────────────────────────────────────────────────────┤
  │ 0x0000_FFFF_FFFF_FFFF                                        │◄── TTBR0 boundary
  │     ┊                                                        │
  │ 0x0000_0000_0000_0000                                        │◄── TTBR0
  └──────────────────────────────────────────────────────────────┘
```

At boot time:
- **TTBR0** → `__pi_init_idmap_pg_dir` (identity map for `.idmap.text`)
- **TTBR1** → `reserved_pg_dir` (empty/zeroed — no valid entries)

The CPU is running at a PA like `0x4000_0000` which, as a VA, falls in the
TTBR0 range (top bits = `0x0000`). All instruction fetches go through TTBR0
using the identity map.

---

## 3. Determining the TTBR — Hardware Circuit Description

In hardware, the VA regime selection is implemented as a combinational
logic circuit in the MMU front-end (Translation Lookaside Buffer controller):

```
VA[63:64-T0SZ]   all zeros?  →  Y → TLB lookup using TTBR0 ASID + VA
                              →  N → check T1SZ

VA[63:64-T1SZ]   all ones?   →  Y → TLB lookup using TTBR1 ASID + VA
                              →  N → raise Translation Fault (level 0)
```

This check happens **before** the TLB is even consulted. It is a 2-cycle
operation in the instruction fetch pipeline (one cycle for comparison, one
for the branch to the correct TTBR path).

---

## 4. The Role of the VA "Hole" as a Security Mechanism

The VA hole is not merely a side effect — it is a **security feature**:

1. **Pointer tag bits are not VA bits:** In the 48-bit VA model, bits [63:48]
   of a pointer must be either all 0 or all 1. Any pointer where these bits
   are neither indicates a corrupt or malicious pointer. The hardware
   immediately faults on such accesses.

2. **Kernel/user separation is enforced in hardware:** User space code cannot
   construct a VA that accidentally or maliciously aliases kernel space, because
   any VA with mixed top bits (non-zero, non-one) faults.

3. **No speculative access to kernel VA from user space:** A speculative load
   to a VA in the hole (e.g., from a buffer over-read) would be killed at the
   MMU stage with a fault signal, before any data is returned.

---

## 5. Impact on the Boot Sequence — Why TTBR1 = `reserved_pg_dir` Is Safe

When `__enable_mmu` sets `TTBR1_EL1 = reserved_pg_dir` (all-zeros page):

The CPU is executing at PA ≈ `0x4000_0000`, which as a VA is in the TTBR0
range. No instruction fetch or data access during `__primary_switch` (until
the `br x8` jump) uses a VA in the TTBR1 range.

The only thing that could trigger a TTBR1 walk at this point would be:
1. The CPU speculatively fetching ahead into TTBR1 range — the branch predictor
   would need to speculate a branch to a high VA. Since `ldr x8, =__primary_switched`
   hasn't executed yet, the predictor has no information about a high-VA branch.
2. An exception (IRQ, FIQ, SError, debug exception) — all masked via DAIF.

With IRQs masked and the exception vector (`VBAR_EL1`) not yet set, any
translation fault from TTBR1 would be unrecoverable. The design ensures this
path is never taken.

---

## 6. The 52-bit VA Extension (FEAT_LPA2, CONFIG_ARM64_VA_BITS_52)

For servers requiring more than 256TB of VA:

```c
#ifdef CONFIG_ARM64_VA_BITS_52
T0SZ = 12   // 52-bit VA: top 12 bits must be 0 or 1
T1SZ = 12
#endif
```

With T0SZ=12:
- TTBR0 range: `0x000F_FFFF_FFFF_FFFF` and below (52-bit user VA)
- TTBR1 range: `0xFFF0_0000_0000_0000` and above (52-bit kernel VA)

The kernel's `PAGE_OFFSET` for 52-bit VA:
```
PAGE_OFFSET = -(2^52) = 0xFFF0_0000_0000_0000
```

**Important implementation note for 52-bit VA:** The ARM hardware for LPA2
uses a different number of initial lookup bits (bits[51:42] instead of [47:39]),
changing the number of page table levels or the number of entries in the first
level. `__primary_switch` automatically handles this through the same
`TCR_EL1` settings computed by `__cpu_setup`.

---

## 7. ASID Participation in TLB Lookup

When TTBR0 or TTBR1 is selected, the TLB lookup includes the **ASID** (Address
Space ID) to distinguish translations from different processes:

- `TTBR0_EL1[63:48]` = ASID (if `TCR_EL1.A1 = 0`: use TTBR0 ASID)
- `TTBR1_EL1[63:48]` = ASID (if `TCR_EL1.A1 = 1`: use TTBR1 ASID)

At boot time:
- Both TTBRs have ASID = 0
- All TLB entries created during boot use ASID 0
- When the first user process runs (`init`), its page tables use ASID 1+

The kernel TTBR1 is typically tagged with ASID 0 (global), meaning TTBR1
entries in the TLB do not participate in ASID-based flushes. `nG=0` in kernel
PTEs (global bit clear = global entry — not ASID-tagged).

---

## 8. Fault Codes for VA Range Violations

When a VA is in the hole (neither TTBR0 nor TTBR1 range):

```
ESR_EL1.EC     = 0b100100   (Data Abort from EL0) or
                  0b100101   (Data Abort from current EL1)
ESR_EL1.DFSC   = 0b000000   (Address Size Fault, level 0)
FAR_EL1        = the faulting virtual address
```

This "Level 0 Translation Fault" (Address Size Fault) indicates the VA was
not in any valid translation regime. The kernel's `do_translation_fault`
handler would catch this and report it as a NULL-pointer-like dereference.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
TCR_EL1 (Translation Control Register, EL1) is a 64-bit system register that controls how the hardware page-table walker interprets addresses. Key fields:
- T0SZ (bits 5:0): number of bits subtracted from 64 to get the size of the TTBR0_EL1 VA region. Linux uses T0SZ=48 for 48-bit user VA (256 TB).
- T1SZ (bits 21:16): same for TTBR1_EL1 (kernel VA). Linux uses T1SZ=48.
- TG0/TG1 (bits 15:14, 31:30): translation granule (4 KB, 16 KB, 64 KB).
- IPS (bits 34:32): intermediate physical address size (physical address bits supported).
- EPD0/EPD1: disable TTBR0/TTBR1 walks (set EPD1=0 and EPD0=0 to enable both).
The hardware reads TCR_EL1 at every TLB miss to know how to parse the VA and how many levels of page tables to walk.

### Kernel Perspective (Linux ARM64)
Linux sets TCR_EL1 in __cpu_setup. The value is built from Kconfig options (VA_BITS, PAGE_SIZE) and CPU feature bits. The TCR value written at boot determines the kernel and user address space split for the entire lifecycle of the system. KASLR does not change TCR; it only changes TTBR1_EL1 to point to a randomly positioned kernel image.

### Memory Perspective (ARMv8 Memory Model)
TCR_EL1 establishes the VA split: addresses with the top T1SZ bits all-ones go through TTBR1_EL1 (kernel), addresses with the top T0SZ bits all-zeros go through TTBR0_EL1 (user). Addresses in between are unmapped (translation fault). This split is the hardware enforcement of the user/kernel address space boundary in the ARMv8 memory model. The TG0/TG1 fields also determine the granularity of permission boundaries (page size).