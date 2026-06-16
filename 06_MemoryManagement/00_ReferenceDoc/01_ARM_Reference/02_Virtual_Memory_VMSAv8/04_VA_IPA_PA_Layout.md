# 02.04 — VA / IPA / PA Layout

> **ARM ARM Reference**: §D5.2.5

---

## 1. Three Address Spaces

| Space | Range field | Range register/source |
|---|---|---|
| **VA** | 64-bit; effective bits = `64 − TxSZ` (max 48 / 52) | `TCR_ELx.T0SZ`, `T1SZ` |
| **IPA** | up to 48 (52 w/ FEAT_LPA) | `VTCR_EL2.PS`, `T0SZ` |
| **PA** | up to 48 (52 w/ FEAT_LPA) | `ID_AA64MMFR0_EL1.PARange` |

`PARange` encoding:

| Value | PA size |
|---|---|
| `0b0000` | 32 bits |
| `0b0001` | 36 bits |
| `0b0010` | 40 bits |
| `0b0011` | 42 bits |
| `0b0100` | 44 bits |
| `0b0101` | 48 bits |
| `0b0110` | 52 bits (FEAT_LPA) |

---

## 2. VA Canonical Form

For `VA_size = 48`:
- Bits[63:48] must equal bit[47] (sign-extension), unless TBI.
- The "canonical hole" between `0x0000_FFFF_FFFF_FFFF + 1` and `0xFFFF_0000_0000_0000 − 1` causes a Translation Fault.

With **TBI=1**, bits[63:56] are ignored for translation (used by MTE / pointer tagging).

---

## 3. Layout Diagram (48-bit VA, 4 KB granule)

```
0xFFFF_FFFF_FFFF_FFFF  ┌──────────────────────────┐
                       │ TTBR1_EL1 (kernel)       │
0xFFFF_0000_0000_0000  ├──────────────────────────┤
                       │  fault hole              │
0x0000_FFFF_FFFF_FFFF  ├──────────────────────────┤
                       │ TTBR0_EL1 (user)         │
0x0000_0000_0000_0000  └──────────────────────────┘
```

---

## 4. IPA Configuration (Stage-2)

`VTCR_EL2.PS` sets the IPA size for the guest. If set smaller than the guest's stage-1 VA, the upper IPA bits must be zero or a fault occurs.

Typical mapping by hypervisor: IPA size = PA size of the host.

```mermaid
flowchart LR
    GVA[Guest VA] -- Stage-1 EL1 --> IPA[Guest IPA] -- Stage-2 EL2 --> PA
```

---

## 5. Worked Example — VA Bit Budget

Configuration: 4 KB granule, 48-bit VA, 4-level walk.

| Bits | Purpose |
|---|---|
| 47:39 | L0 index (512 entries) |
| 38:30 | L1 index |
| 29:21 | L2 index |
| 20:12 | L3 index |
| 11:0  | offset within 4 KB page |

Total = 9+9+9+9+12 = 48 bits ✓

---

## 6. PA Size Implications

- A `PARange` of 40 bits means TTBR addresses, PTEs, and FAR all must fit within 40 bits — upper bits must be zero.
- TTBR0/1 fields reserve `[47:1]` for the table base, but only `[PARange-1:1]` are valid.
- Stage-2 IPA must not exceed PA size (a guest cannot address beyond physical reality).

---

## 7. Pitfalls

1. **Setting `T0SZ` too small** (large VA) on hardware that doesn't support it → translation faults at boot.
2. **Overlooking the canonical hole** when manipulating raw pointers (e.g., MTE tag injection in [56:63]).
3. **IPA > PA** → hypervisor must reject guest configuration.
4. **Mixing 48-bit and 52-bit modes** — requires FEAT_LVA + FEAT_LPA and only with 64 KB granule.

---

## 8. Interview Q&A

**Q1. What's the maximum VA on ARMv8?**
48 bits standard, 52 bits with FEAT_LVA + 64 KB granule.

**Q2. What's the canonical hole?**
Region between TTBR0's top and TTBR1's bottom; access there causes a Translation Fault.

**Q3. How is VA size set?**
`TCR_ELx.T0SZ` for the low half and `T1SZ` for the high half. Effective bits = 64 − TxSZ.

**Q4. What does TBI ignore?**
Bits [63:56] of the VA for translation purposes.

**Q5. Why might PA size be smaller than VA size?**
Real silicon may only wire e.g. 40 bits to DRAM. The architecture allows up to 52 but implementations may be smaller.

**Q6. IPA vs PA — give a use case where they differ.**
Under KVM, a Linux guest sees IPA `0x4000_0000` (its "RAM start") but the host has mapped that to PA `0x9_8000_0000` via stage-2.

---

## 9. Cross-refs

- [01 VMSA overview](01_VMSA_Overview_and_Address_Spaces.md)
- [03 Granules](03_Translation_Granules_4K_16K_64K.md)
- [09.02 IPA / VMID](../09_Virtualization_Memory/02_IPA_Space_and_VMID.md)
