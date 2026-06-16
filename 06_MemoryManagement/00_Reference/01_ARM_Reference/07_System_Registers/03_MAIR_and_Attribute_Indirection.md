# 07.03 — MAIR_ELx (Detail — Register View)

> **ARM ARM Reference**: §D13.2.95

This document focuses on the **register-level** detail. For the conceptual treatment of attribute indirection, see [01.03 MAIR and Attribute Encoding](../01_Memory_Model/03_MAIR_and_Attribute_Encoding.md).

---

## 1. Per-EL MAIR Registers

| Register | Regime |
|---|---|
| `MAIR_EL1` | EL1&0 stage-1 |
| `MAIR_EL2` | EL2 (and EL2&0 in VHE) |
| `MAIR_EL3` | EL3 |

There is no `MAIR_EL0` — EL0 inherits from the regime that contains it.

There is an `AMAIR_ELx` (Auxiliary MAIR) for IMPLEMENTATION DEFINED extensions; typically 0.

---

## 2. Bit Layout

64 bits total → eight 8-bit attribute slots `Attr0`…`Attr7`.

```
 63       56 55       48 47       40 39       32 31       24 23       16 15        8 7         0
+----------+----------+----------+----------+----------+----------+----------+----------+
|  Attr7   |  Attr6   |  Attr5   |  Attr4   |  Attr3   |  Attr2   |  Attr1   |  Attr0   |
+----------+----------+----------+----------+----------+----------+----------+----------+
```

PTE `AttrIdx[2:0] = n` ⇒ access uses `MAIR.Attr<n>`.

---

## 3. Slot Encoding Recap

| Pattern in 8-bit slot | Type |
|---|---|
| `0b0000_RRRR` | Device (R selects sub-type) |
| `0b0100_0100` | Normal NC inner+outer |
| `0bOOOO_IIII` (other non-zero) | Normal — Outer/Inner cacheability |
| `0bF0` family | Normal Tagged (MTE) |

Detailed encoding table is in [01.03](../01_Memory_Model/03_MAIR_and_Attribute_Encoding.md#3-8-bit-attr-encoding).

---

## 4. Updating MAIR

```asm
    ldr   x0, =MAIR_VAL
    msr   mair_el1, x0
    ; existing TLB entries hold *resolved* attrs — invalidate
    tlbi  vmalle1is
    dsb   ish
    isb
```

Without TLBI + ISB, old entries may continue to apply old attributes.

---

## 5. Stage-2 — No MAIR

Stage-2 PTEs encode the attribute directly in `MemAttr[3:0]`. No analog of MAIR.

| MemAttr (stage-2) | Meaning |
|---|---|
| `0000` | Device-nGnRnE |
| `0001` | Device-nGnRE |
| `0010` | Device-nGRE |
| `0011` | Device-GRE |
| `01XX` | Normal — Outer NC, Inner = XX |
| `10XX` | Normal — Outer WT, Inner = XX |
| `11XX` | Normal — Outer WB, Inner = XX |

(Inner XX: `01`=NC, `10`=WT, `11`=WB.)

---

## 6. Pitfalls

1. **Forgetting TLBI when MAIR changes** — silent attribute mismatches.
2. **Slot 0 == Device-nGnRnE by Linux convention but not architecture** — don't assume across kernels.
3. **Programming `MAIR_EL2` to differ from EL1 expectations** — under VHE, EL2's MAIR is what counts; misconfigure → guest's attributes silently wrong.

---

## 7. Interview Q&A

**Q1. How many attribute slots per MAIR?**
Eight (3-bit AttrIdx in PTE).

**Q2. What's the per-slot width?**
8 bits.

**Q3. What action is required after writing MAIR?**
TLB invalidate (TLBI), DSB, then ISB.

**Q4. Does stage-2 use a MAIR?**
No; stage-2 PTEs encode the memory attribute directly.

**Q5. What's `0xFF` decode?**
Normal, Inner+Outer Write-Back, RA+WA, Non-transient — full cache enable.

**Q6. What's the difference between MAIR and AMAIR?**
AMAIR is IMPL DEF auxiliary extensions; typically zero. Architecture relies on MAIR.

---

## 8. Cross-refs

- [01.03 MAIR concept & full encodings](../01_Memory_Model/03_MAIR_and_Attribute_Encoding.md)
- [01.01 Memory types](../01_Memory_Model/01_Memory_Types_Normal_Device.md)
- [03.04 Stage1/2](../03_Page_Tables_and_Translation/04_Stage1_vs_Stage2_Translation.md)
