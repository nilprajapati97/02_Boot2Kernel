# 03.01 — Translation Table Descriptor Formats

> **ARM ARM Reference**: §D5.3

---

## 1. Four Descriptor Kinds

Every 64-bit entry in a VMSAv8 translation table is one of:

| Type | bits[1:0] | Meaning |
|---|---|---|
| **Invalid** | `0bX0` | No mapping → translation fault if accessed |
| **Block** (not at last level) | `0b01` | Maps a large naturally-aligned region directly |
| **Table** (not at last level) | `0b11` | Pointer to a next-level translation table |
| **Page** (at last level only) | `0b11` | Maps one granule-sized page |
| **Reserved** | `0b01` at L3 (4 KB) | Invalid |

---

## 2. Block / Page Descriptor Layout (4 KB granule, L3 page)

```
 63   62 61 60   59 58:55 54  53  52  51   50 49:48 47:12 11  10  9:8  7:6  5  4:2  1  0
+-----+-----+-----+-----+----+----+----+-----+-----+-------+----+----+----+----+---+-----+--+--+
| NSE | PBHA| Res |Ign  | XN | PXN| C  | DBM |  Res| OA[47:12]|nG  | AF | SH | AP | NS|AtIdx|1 |V|
+-----+-----+-----+-----+----+----+----+-----+-----+-------+----+----+----+----+---+-----+--+--+
```

(Layout simplified; exact bit positions vary by feature set — FEAT_HAFDBS, FEAT_BBM, etc.)

### Key fields

| Field | Bits | Purpose |
|---|---|---|
| **V** (Valid) | [0] | 1 = valid descriptor |
| **Type bit** | [1] | distinguishes block vs page/table |
| **AttrIdx** | [4:2] | Index into `MAIR_ELx` |
| **NS** | [5] | Non-secure bit (stage-1 EL3 only) |
| **AP[2:1]** | [7:6] | Access permissions (R/W, EL0 access) |
| **SH** | [9:8] | Shareability (NSH/ISH/OSH) |
| **AF** (Access Flag) | [10] | Set on first access (HW or SW) |
| **nG** | [11] | Not-global → tagged with ASID |
| **OA** (Output Address) | [47:12] | Physical (or IPA for stage-1) address |
| **DBM** | [51] | Dirty Bit Modifier (FEAT_HAFDBS) |
| **Contiguous** | [52] | Contiguous hint |
| **PXN** | [53] | Privileged Execute-Never |
| **XN/UXN** | [54] | (Unprivileged) Execute-Never |
| **PBHA** | [62:59] | Page-Based Hardware Attributes (FEAT_HPDS2) |
| **NSE** | [11] (EL3 stage-1) | Non-secure encoding extension |

---

## 3. Table Descriptor Layout

```
 63    62:61 60 59 58:52 51:48 47:12             11:2 1 0
+----+------+----+----+-------+-----+----------+-----+--+--+
|NSTbl|APTbl|XNTbl|PXNTbl|Res |IGNORE| Next-table PA|IGN |1 |V|
+----+------+----+----+-------+-----+----------+-----+--+--+
```

Table-level permission bits **further restrict** all entries below — useful for hierarchical permissions:

| Field | Effect |
|---|---|
| `APTable[1:0]` | Restricts AP of all child entries (e.g., force RO, force EL1-only) |
| `XNTable` | Forces XN on all children |
| `PXNTable` | Forces PXN on all children |
| `NSTable` | All children resolve in non-secure PA space |

---

## 4. Stage-2 Descriptors (small differences)

Stage-2 PTEs carry **explicit memory attributes** (no MAIR), encoded in `MemAttr[3:0]` field, and stage-2 access permissions `S2AP[1:0]` (different encoding than stage-1 AP).

| Field | Stage-1 | Stage-2 |
|---|---|---|
| Attrs | `AttrIdx` (3 bits → MAIR) | `MemAttr` (4 bits direct) |
| Perms | `AP[2:1]` + UXN/PXN | `S2AP[1:0]` + XN[1:0] |
| ASID | yes (`nG`) | n/a |
| VMID | n/a | yes (in VTTBR) |

---

## 5. Diagram — descriptor decode

```mermaid
flowchart TD
    Entry[64-bit entry] --> B0{bit[0] V?}
    B0 -- 0 --> Inv[Invalid → fault]
    B0 -- 1 --> B1{bit[1]?}
    B1 -- 0 --> Block[Block descriptor]
    B1 -- 1 --> Lvl{at last level?}
    Lvl -- yes --> Page[Page descriptor]
    Lvl -- no --> Table[Table descriptor]
```

---

## 6. Worked Example — Decoding a Page Descriptor

```
PTE = 0x0068_0000_4000_0F43
```

Bits[1:0] = `0b11` → page (assuming L3).
Bits[4:2] = `0b000` → AttrIdx = 0 → Device-nGnRnE (in Linux MAIR).
Bits[7:6] = `0b11` → AP = `0b11` → EL0 RW.
Bits[9:8] = `0b11` → SH = ISH.
Bit[10]   = `1`    → AF set.
Bit[11]   = `0`    → global mapping.
Bits[47:12] = output PA[47:12].
Bit[53] = 0 → not PXN; bit[54] = 0 → not XN.

Verdict: Device-nGnRnE, EL0 RW, ISH, executable.

---

## 7. Pitfalls

1. **Putting a block descriptor at L3 (4 KB)** — `0b01` at L3 is reserved → fault.
2. **Forgetting Access Flag** — if HW AF update not supported, first access faults.
3. **Misusing `APTable`** — write-only intent at parent forces RO on all children even if children look RW.
4. **Cross-attribute child entries** — table-level attributes are AND-restrictive only, not OR-permissive.
5. **Encoding stage-1 attrs into a stage-2 PTE** — different field layout.

---

## 8. Interview Q&A

**Q1. What are the four descriptor types?**
Invalid, Block, Table, Page.

**Q2. When is the type bit interpreted as "page" vs "table"?**
At the last walk level (e.g., L3 for 4K) `0b11` means Page; at non-last levels `0b11` means Table.

**Q3. What does `APTable` do?**
At a table descriptor, restricts AP for all descendant entries — hierarchical perm filter.

**Q4. What's the Access Flag and who sets it?**
Bit[10] of a Block/Page PTE. Either HW (FEAT_HAFDBS) sets on first access, or software is responsible (else first access faults).

**Q5. What does `nG` do?**
Marks a mapping as "not global" — TLB entry tagged with ASID. Clear `nG` (=global) for kernel mappings.

**Q6. How does stage-2 encode memory attributes?**
Directly in PTE's `MemAttr[3:0]` field — no MAIR lookup.

**Q7. What's the contiguous bit for?**
Hints to the TLB that this PTE is part of a naturally-aligned contiguous group of 16/32/128 — can be coalesced into one TLB entry.

---

## 9. Cross-refs

- [02 Multi-level walk](02_Multi_Level_Page_Walk.md)
- [03 Block vs Page](03_Block_vs_Page_Mappings.md)
- [05 Access Flag / Dirty](05_Access_Flag_and_Dirty_State.md)
- [06 Permissions](06_Permission_Checks_AP_UXN_PXN.md)
