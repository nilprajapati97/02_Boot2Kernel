# 07.01 — SCTLR_ELx (System Control Register)

> **ARM ARM Reference**: §D13.2.118

---

## 1. Purpose

`SCTLR_ELx` is the master control register for **MMU, caches, alignment, endianness, WXN policy**, and various exception/checking behaviors at each Exception Level.

One register per EL: `SCTLR_EL1`, `SCTLR_EL2`, `SCTLR_EL3`.

---

## 2. Key Bits (subset most asked about)

| Bit | Name | Meaning when set |
|---|---|---|
| 0  | **M**   | MMU enable |
| 1  | A   | Alignment check enable (faults on misaligned access) |
| 2  | **C**   | Data cache enable (cacheable accesses use cache) |
| 3  | SA  | EL1 SP alignment check |
| 4  | SA0 | EL0 SP alignment check |
| 5  | CP15BEN | EL1 AArch32 CP15 barrier enable (legacy) |
| 6  | nAA | Non-aligned access fault for LDAPR (FEAT_LSE2) |
| 7  | ITD | IT-disable (AArch32) |
| 8  | SED | Setend disable (AArch32) |
| 11 | EOS | Exception entry context synchronisation |
| 12 | **I** | Instruction cache enable |
| 14 | DZE | Allow EL0 to use DC ZVA |
| 15 | UCT | Allow EL0 read of CTR_EL0 |
| 16 | nTWI | Don't trap WFI from EL0 |
| 18 | nTWE | Don't trap WFE from EL0 |
| 19 | **WXN** | Writable→Execute-Never (forces XN on any writable page) |
| 22 | EIS | Exception entry "implicit ISB" |
| 23 | SPAN | Set Privileged Access Never on exception entry |
| 24 | E0E | EL0 endianness (0=LE, 1=BE) |
| 25 | EE  | EL1 endianness |
| 26 | UCI | Allow EL0 cache maintenance ops |
| 35 | **EnIA / EnIB / EnDA / EnDB** | Pointer Authentication enables |
| 37 | BT0 / BT1 | BTI guards |
| 42 | DSSBS | Default SSBS for software (Spectre-v4) |

---

## 3. The Big Three for MMU/Cache

| Bit | Effect when 0 | Effect when 1 |
|---|---|---|
| **M** (0) | MMU off — VA=PA, Device-nGnRnE | MMU on — translation/perm checks apply |
| **C** (2) | Cacheable accesses behave as Non-cacheable | Data caches active |
| **I** (12) | Cacheable instruction fetches Non-cacheable | I-cache active |

Linux boot sequence flips all three roughly simultaneously after preparing the page tables:

```asm
    msr  ttbr0_el1, x0
    msr  ttbr1_el1, x1
    msr  tcr_el1,   x2
    msr  mair_el1,  x3
    isb
    mrs  x4, sctlr_el1
    orr  x4, x4, #(SCTLR_M | SCTLR_C | SCTLR_I)
    msr  sctlr_el1, x4
    isb                          ; mandatory — context sync
```

---

## 4. Diagram — boot enable sequence

```mermaid
sequenceDiagram
    participant SW
    participant SysRegs
    participant MMU
    SW->>SysRegs: write TTBR/TCR/MAIR
    SW->>SysRegs: ISB
    SW->>SysRegs: set SCTLR.{M,C,I} = 1
    SW->>SysRegs: ISB
    Note over MMU: MMU now active; subsequent fetches translated
```

---

## 5. WXN — Writable XOR Executable

`SCTLR.WXN=1` enforces: any page mapped writable is automatically treated as XN. Useful for hardening. Linux generally sets this for kernel mappings.

---

## 6. Pitfalls

1. **Enabling M without ISB** — fetches in flight may use old VA=PA mapping.
2. **Forgetting C while enabling MMU** — performance disaster; everything goes to DRAM.
3. **`SCTLR.A=1`** turns alignment checks on for *all* memory; some code paths break.
4. **Changing endianness (`EE` / `E0E`)** mid-execution is essentially impossible safely.
5. **Setting WXN after some kernel text is already mapped writable** — those mappings effectively become non-executable until repaired.

---

## 7. Interview Q&A

**Q1. Which bit enables the MMU?**
`SCTLR_ELx.M` (bit 0).

**Q2. What does SCTLR.C control?**
Data cache enable for cacheable regions. With C=0, cacheable attrs are downgraded to non-cacheable.

**Q3. What's WXN?**
A policy bit forcing writable mappings to be XN — defense in depth against code injection.

**Q4. Why is ISB required after writing SCTLR?**
Pipeline may have already fetched/translated instructions under old config. ISB flushes and re-fetches.

**Q5. What's `SCTLR.SPAN`?**
Controls whether `PSTATE.PAN` is automatically set on exception entry — Linux clears SPAN so PAN is enabled on entry to EL1.

**Q6. What controls EL0 read access to CTR_EL0?**
`SCTLR_EL1.UCT` (bit 15).

---

## 8. Cross-refs

- [02 TTBR / TCR](02_TTBR0_TTBR1_TCR.md)
- [03 MAIR](03_MAIR_and_Attribute_Indirection.md)
- [06 Permissions PAN/UAO](../03_Page_Tables_and_Translation/06_Permission_Checks_AP_UXN_PXN.md)
